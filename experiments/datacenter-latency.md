# Pick a relay location using all four home connections

Use `tests/measure_datacenter_latency.py` with Python 3.9 or newer and the operating
system's `ping`. No Python packages or administrator privileges are normally needed.
Supports macOS, Linux, and English-language Windows ping output. On Windows, use
`py -3` instead of `python3`. Linux needs its usual ping utility and ICMP permissions.

## Each player

Send everyone the standalone Python file. From its folder, run:

```sh
python3 measure_datacenter_latency.py run --player louis-la --out louis-la-evening1.json
```

Use unique names, e.g. `louis-la`, `player2-la`, `player3-sd`, `player4-tn`.
The default sends 300 small IPv4 pings per candidate, about one per second, with
all six candidates running concurrently. Allow roughly five minutes (longer if
packets time out). Results stay in the named local JSON file; nothing is uploaded.
The file includes resolved destination IPs, raw ping output, samples, loss,
median, p95, p99, and maximum RTT. An existing output file is never overwritten.
Review raw output before sharing if local network addresses are sensitive.

Run on the actual gaming computer and connection, preferably wired. Avoid a VPN
unless you will play through it. Run all four tests around the same time, during
your usual gaming hours. Repeat on at least three evenings with new output names.
Do not saturate the link with a download speed test while this test is running.

Default public targets (checked 2026-10-25):

| Candidate | Host |
| --- | --- |
| Linode Dallas | speedtest.dallas.linode.com |
| Linode Los Angeles | speedtest.los-angeles.linode.com |
| Vultr Dallas | tx-us-ping.vultr.com |
| Vultr Los Angeles | lax-ca-us-ping.vultr.com |
| Vultr Atlanta | ga-us-ping.vultr.com |
| Vultr Chicago | il-us-ping.vultr.com |

Sources: [Akamai's ping guidance](https://techdocs.akamai.com/cloud-computing/docs/how-to-choose-a-data-center),
[Dallas speed test](https://speedtest.dallas.linode.com/), and Vultr's
[LA](https://lax-ca-us-ping.vultr.com/), [Dallas](https://tx-us-ping.vultr.com/),
[Atlanta](https://ga-us-ping.vultr.com/), [Chicago](https://il-us-ping.vultr.com/) looking glasses.
The Linode LA web endpoint timed out during research; the tool reports failures
explicitly. Public targets may move or stop responding. Failure is not proof that
a rented VM in that region will perform badly.

## Combine results

Collect one JSON file per player from the same session, then run:

```sh
python3 measure_datacenter_latency.py compare louis-la-evening1.json player2-la-evening1.json player3-sd-evening1.json player4-tn-evening1.json
```

The comparison requires four distinct players, excludes candidates missing usable
samples for anyone, and flags differing destination IPs. Compare each evening
separately; repeated reports from one player cannot stand in for another player.

For each candidate it shows the slowest pair's estimated typical one-way relay
delay: `(median RTT A + median RTT B) / 2`. It also reports the highest individual
loss percentage and a tail proxy: the largest `(p95 RTT A + p95 RTT B) / 2` over
all pairs. The tail proxy sorts the shortlist; it is **not a measured relay p95**.
Both estimates assume roughly symmetric routes and omit relay processing time.
Inspect loss and individual reports before selecting; a quick median with frequent
spikes or drops is undesirable. A candidate with no replies is excluded, not scored
as zero latency. OS permission and parsing failures are recorded as errors.

## Confirm finalists on actual servers

Public ICMP tests screen routes, not the complete game path. ICMP may be filtered
or deprioritized, and a provider's test host may have a different route from a VM.
Rent short-lived VMs in the top two provider/location combinations. Repeat using
their actual IPv4 addresses, supplying the same target arguments to each player:

```sh
python3 measure_datacenter_latency.py run --player louis-la --out louis-finalists.json --target candidate-a=SERVER_A_IP --target candidate-b=SERVER_B_IP
```

Then use the game's UDP/ENet transport for a four-player playtest on both. Compare
input age, arrival gaps, packet loss, rollback depth, and stalls during actual
battles. This script does not implement a UDP echo service or measure one-way
latency directly. A ping-only ranking is provisional until that confirmation.

Regression checks: `python3 -m unittest discover -s tests -p test_datacenter_latency.py`.
