#!/usr/bin/env python3
"""Compare IPv4 ICMP routes for a small input-relay server. Python 3.9+, no packages."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import itertools
import json
import math
import os
from pathlib import Path
import platform
import re
import socket
import statistics
import subprocess


# Public provider test hosts; see experiments/datacenter-latency.md for sources.
TARGETS = {
    "linode-dallas": "speedtest.dallas.linode.com",
    "linode-la": "speedtest.los-angeles.linode.com",
    "vultr-dallas": "tx-us-ping.vultr.com",
    "vultr-la": "lax-ca-us-ping.vultr.com",
    "vultr-atlanta": "ga-us-ping.vultr.com",
    "vultr-chicago": "il-us-ping.vultr.com",
}


def percentile(values, fraction):
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def parse_ping(output, system, count):
    """Reject incomplete/unrecognized runs instead of treating errors as network loss."""
    if system == "Windows":
        summary = re.search(r"Sent = (\d+), Received = (\d+), Lost = (\d+)", output)
    else:
        summary = re.search(r"(\d+) packets transmitted, (\d+) (?:packets )?received", output)
    if not summary or int(summary[1]) != count:
        raise ValueError("Incomplete or unrecognized ping output (Windows requires English output).")
    if re.search(r"Operation not permitted|Permission denied|General failure", output, re.I):
        raise ValueError("The operating system blocked ping; this is not packet loss.")
    values, seen = [], set()
    for line in output.splitlines():
        if "DUP!" in line or not re.search(r"ttl[= ]", line, re.I):
            continue
        match = re.search(r"time\s*([=<])\s*([\d.]+)\s*ms", line, re.I)
        if not match:
            continue
        seq = re.search(r"icmp_seq[= ](\d+)", line)
        if seq and seq[1] in seen:
            continue
        if seq:
            seen.add(seq[1])
        # Windows reports <1 ms: retain the conservative upper bound, 1 ms.
        values.append(float(match[2]))
    if len(values) > count:
        raise ValueError("Unexpected extra replies.")
    # Windows counts some ICMP error replies as received; only echo RTTs count here.
    if system != "Windows" and len(values) != int(summary[2]):
        raise ValueError("Reply samples disagree with ping summary.")
    return values


def summarize(values, count):
    stats = {"received": len(values), "loss_pct": 100 * (count - len(values)) / count}
    if values:
        stats.update(median_ms=statistics.median(values), p95_ms=percentile(values, .95),
                     p99_ms=percentile(values, .99), max_ms=max(values))
    return stats


def measure(target, count):
    label, host = target
    result = {"label": label, "host": host, "sent": count}
    system = platform.system()
    try:
        ip = socket.gethostbyname(host)  # Pin one IPv4 address for the whole run.
        result["ip"] = ip
        if system == "Windows":
            command = ["ping", "-4", "-n", str(count), "-w", "1000", ip]
        elif system in ("Darwin", "Linux"):
            command = ["ping", "-n", "-c", str(count), "-i", "1", "-W",
                       "1000" if system == "Darwin" else "1", ip]
        else:
            raise ValueError("Supported systems: macOS, Linux, Windows.")
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, errors="replace", env={**os.environ, "LC_ALL": "C"},
                                   timeout=count * 2 + 15)
        result["raw_output"] = completed.stdout
        values = parse_ping(completed.stdout, system, count)
        result.update(samples_ms=values, **summarize(values, count))
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        result["error"] = str(error)
    return result


def compare(reports, player_count):
    if len(reports) != player_count or len({r["player"] for r in reports}) != player_count:
        raise ValueError(f"Provide one report from each of {player_count} distinct players per session.")
    if any(r.get("version") != 1 or r.get("protocol") != "ICMP/IPv4" for r in reports):
        raise ValueError("Unsupported report format.")
    maps = [{(t["label"], t["host"]): t for t in r["targets"]} for r in reports]
    rows, excluded = [], []
    for key in sorted(set().union(*(set(m) for m in maps))):
        targets = [m.get(key) for m in maps]
        if any(t is None or "error" in t or not t.get("samples_ms") for t in targets):
            excluded.append(key[0])
            continue
        stats = [summarize(t["samples_ms"], t["sent"]) for t in targets]
        pairs = list(itertools.combinations(range(player_count), 2))
        worst = max(pairs, key=lambda ij: sum(stats[i]["median_ms"] for i in ij))
        rows.append({
            "label": key[0],
            "pair": "/".join(reports[i]["player"] for i in worst),
            "median_estimate": sum(stats[i]["median_ms"] for i in worst) / 2,
            # This is a ranking proxy, NOT the measured p95 of a relayed packet.
            "tail_proxy": max(sum(stats[i]["p95_ms"] for i in pair) / 2 for pair in pairs),
            "loss": max(s["loss_pct"] for s in stats),
            "different_ips": len({t["ip"] for t in targets}) > 1,
        })
    return sorted(rows, key=lambda r: r["tail_proxy"]), excluded


def positive_count(value):
    count = int(value)
    if not 2 <= count <= 3600:
        raise argparse.ArgumentTypeError("Use 2–3600 samples/players.")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    run = sub.add_parser("run", help="Probe all candidates concurrently; save a local JSON report.")
    run.add_argument("--player", required=True, help="Unique nickname, e.g. louis-la")
    run.add_argument("--count", type=positive_count, default=300)
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--target", action="append", metavar="LABEL=HOST",
                     help="Replace defaults; repeat for each candidate (same list for all players).")
    group = sub.add_parser("compare", help="Compare one report per player from the same session.")
    group.add_argument("reports", nargs="+", type=Path)
    group.add_argument("--players", type=positive_count, default=4)
    args = parser.parse_args()
    try:
        if args.mode == "compare":
            rows, excluded = compare([json.loads(p.read_text()) for p in args.reports], args.players)
            print("Candidate          worst-pair median estimate  tail proxy  worst loss  worst median pair")
            for row in rows:
                print(f"{row['label']:19} {row['median_estimate']:9.1f} ms"
                      f"                 {row['tail_proxy']:6.1f} ms   {row['loss']:6.2f}%  {row['pair']}"
                      + (" [different destination IPs]" if row["different_ips"] else ""))
            if excluded:
                print("Excluded (missing/failed/unreachable for a player): " + ", ".join(excluded))
            print("Sorted by tail proxy; inspect loss before shortlisting. Estimates assume symmetric routes.")
            print("Tail proxy = worst pair's (p95 RTT A + p95 RTT B)/2; NOT measured relay p95.")
            if not rows:
                raise ValueError("No candidate has usable samples from every player.")
            return
        targets = TARGETS
        if args.target:
            targets = {}
            for target in args.target:
                label, separator, host = target.partition("=")
                if not separator or not label or not host or label in targets:
                    raise ValueError("Each target needs a unique LABEL=HOST.")
                targets[label] = host
        args.out.parent.mkdir(parents=True, exist_ok=True)
        # Refuse to overwrite a previous session's results.
        with args.out.open("x", encoding="utf-8") as output:
            started = datetime.now(timezone.utc).isoformat()
            print(f"Testing {len(targets)} candidates, {args.count} probes each (~{args.count // 60 + 1} minutes).",
                  flush=True)
            with ThreadPoolExecutor(max_workers=min(12, len(targets))) as pool:
                results = list(pool.map(lambda t: measure(t, args.count), targets.items()))
            json.dump({"version": 1, "protocol": "ICMP/IPv4", "player": args.player,
                       "started_utc": started, "system": platform.system(), "targets": results}, output, indent=2)
            output.write("\n")
        for r in results:
            detail = r.get("error") or (f"loss {r['loss_pct']:.2f}%, median {r.get('median_ms', 'n/a')} ms, "
                                         f"p95 {r.get('p95_ms', 'n/a')} ms")
            print(f"{r['label']}: {detail}")
        print(f"Saved {args.out}")
        if not any(r.get("samples_ms") for r in results):
            raise ValueError("No candidate replied. Check the saved errors and ICMP access.")
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"Error: {error}\n")


if __name__ == "__main__":
    main()
