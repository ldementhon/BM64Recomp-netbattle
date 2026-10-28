import unittest
from measure_datacenter_latency import compare, parse_ping, summarize


class LatencyTests(unittest.TestCase):
    def test_mac_duplicates_and_loss(self):
        output = """64 bytes from 1.2.3.4: icmp_seq=0 ttl=55 time=12.000 ms
64 bytes from 1.2.3.4: icmp_seq=0 ttl=55 time=12.100 ms (DUP!)
Request timeout for icmp_seq 1
64 bytes from 1.2.3.4: icmp_seq=2 ttl=55 time=18.000 ms
3 packets transmitted, 2 packets received, +1 duplicates, 33.3% packet loss
"""
        samples = parse_ping(output, "Darwin", 3)
        self.assertEqual(samples, [12, 18])
        self.assertAlmostEqual(summarize(samples, 3)["loss_pct"], 100 / 3)

    def test_linux(self):
        output = """64 bytes from 1.2.3.4: icmp_seq=1 ttl=53 time=20.1 ms
64 bytes from 1.2.3.4: icmp_seq=2 ttl=53 time=21.2 ms
2 packets transmitted, 2 received, 0% packet loss, time 1001ms
"""
        self.assertEqual(parse_ping(output, "Linux", 2), [20.1, 21.2])

    def test_windows_errors_are_not_successes(self):
        output = """Reply from 1.2.3.4: bytes=32 time<1ms TTL=64
Reply from 1.2.3.4: bytes=32 time=40ms TTL=64
Reply from 192.168.1.1: Destination host unreachable.
Packets: Sent = 3, Received = 3, Lost = 0 (0% loss),
"""
        self.assertEqual(parse_ping(output, "Windows", 3), [1, 40])

    def test_failures_are_not_loss_measurements(self):
        for output in ["ping: unknown host", "2 packets transmitted, 2 received",
                       "Operation not permitted\n2 packets transmitted, 0 packets received"]:
            with self.assertRaises(ValueError):
                parse_ping(output, "Darwin", 2)
        self.assertEqual(parse_ping("2 packets transmitted, 0 received", "Linux", 2), [])

    def test_tail_and_loss(self):
        stats = summarize(list(range(1, 101)), 125)
        self.assertEqual(stats["median_ms"], 50.5)
        self.assertEqual(stats["p95_ms"], 95)
        self.assertEqual(stats["p99_ms"], 99)
        self.assertEqual(stats["loss_pct"], 20)

    def test_compare_worst_pair_not_average_and_require_all_players(self):
        reports = []
        # LA has lower overall average RTT, but a worse slowest pair than Dallas.
        for i, (la, dallas) in enumerate(zip([10, 10, 10, 90], [40, 40, 40, 40])):
            targets = [{"label": label, "host": label, "ip": "1.2.3.4", "sent": 2,
                        "samples_ms": [value, value]} for label, value in [("la", la), ("dallas", dallas)]]
            reports.append({"version": 1, "protocol": "ICMP/IPv4", "player": str(i), "targets": targets})
        rows, excluded = compare(reports, 4)
        self.assertEqual(excluded, [])
        self.assertEqual([r["label"] for r in rows], ["dallas", "la"])
        self.assertEqual(rows[1]["median_estimate"], 50)
        reports[0]["targets"][0]["samples_ms"] = []
        self.assertEqual(compare(reports, 4)[1], ["la"])
        with self.assertRaises(ValueError):
            compare(reports[:3], 4)
        reports[0]["player"] = reports[1]["player"]
        with self.assertRaises(ValueError):
            compare(reports, 4)


if __name__ == "__main__":
    unittest.main()
