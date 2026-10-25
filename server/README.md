# Private battle lobby prototype

Build independently of the game, ROM, renderer, and game submodules:

```sh
cmake -S server -B build/lobby -DCMAKE_BUILD_TYPE=Release
cmake --build build/lobby -j 4
ctest --test-dir build/lobby --output-on-failure
build/lobby/bm64-lobby 127.0.0.1 6464 CLIENT_FINGERPRINT_HEX
```

ENet 1.3.18 is pinned to upstream commit
`2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`. CMake fetches it during configuration.
The final argument must be 64 hexadecimal digits identifying the accepted client
build. The server compares the complete 32-byte value before allocating a seat.
Release fingerprint generation and the user-facing game client are still pending;
the test value is only a fixture, not a compatibility certification.

The server owns one room and serializes membership, Ready, and Start. Roster
revisions change on membership changes and return to Lobby; Ready flag updates
keep the revision so simultaneous Ready requests for the same members remain
valid. A failed or disconnected player aborts loading/play for everyone. An
ordinary completed match keeps connections and clears Ready. Every match gets a
new ID. The start epoch is server monotonic milliseconds. The connection client
uses the lowest-RTT estimate among eight recent clock samples, requires at least
three fresh samples before acknowledging loading, and cancels a late start. Frame
numbers and bounded simulation lead still need the input protocol; clock estimates
alone cannot synchronize gameplay over asymmetric routes.

Control messages use reliable ordered channel 0, explicit big-endian fields,
protocol version 1, and a 1,199-byte maximum. Channels 1–4 carry separate unreliable
sequenced input streams, with 32 recent inputs per upload. There are eight transport slots (four
playable seats plus pending/rejected joins), a five-second handshake deadline,
120 incoming messages per connection per second, a 64-KiB incoming wait
limit, and a 128-command outgoing queue limit. Slow or invalid peers are removed.
Loading has a 30-second deadline; partial finish agreement and stalled input
confirmation each have five seconds. Input repeats are forwarded immediately,
even if previously accepted, so a receiver can repair a lost relay while stalled.

The tests exercise actual loopback UDP connections for 2/3/4 clients, simultaneous
joins, full-room and incompatible-build rejection, stale readiness, repeated
start/finish, load failure, and loading disconnect. Pure room tests additionally
cover timeout, digest mismatch, stale matches, P1 permissions, and playing
disconnect. Test programs keep assertions enabled in Release builds.

`sh tests/run_lobby_tests.sh` runs all checks. On macOS,
`BM64_LOBBY_REAL_GAME=1 sh tests/run_lobby_tests.sh` additionally launches two
real games through separate persistent connections. It compares their initial
states, starts/cancels twice, and deliberately delivers a third start too late.
Both owners retain identities and clean up settings. This two-human/two-CPU
fixture proves startup/cancel, not normal match completion or live inputs.
The ordinary test uses disposable fixture children to verify finish agreement
after exchanging assigned inputs through the private IPC channel and ENet relay,
and verifies that no UDP sockets are inherited by game processes. Private channel
version 2 queues at most 2,048 fixed 240-byte records, supports partial nonblocking
writes, and fails if that bound is exceeded. The owner drains at most 128 child
records per poll. These input records are separate from checkpoints. Windows game-process
ownership remains unimplemented; its tests are excluded on that platform.

`input_network` runs four production client connections through actual ENet input
channels. It independently delays, drops, and duplicates received relays at the
client delivery boundary, checks corrected state against an oracle, and verifies
confirmed completion and stalled-match timeout. This is not OS-level packet
shaping or a ROM rollback test. Raw connection tests reject slot impersonation
and conflicting replacements. See [input frame rules](INPUT_PROTOCOL.md).

This is not yet a playable game client. No ROM, checkpoint, or native pointer is
sent. The server checks finish-frame agreement and requires that frame to be
confirmed by every human receiver. The game adapter must still detect completion
after correcting its actual simulation to that confirmed boundary.
No deployment has been performed. For remote private tests, bind the chosen
interface and restrict its UDP port to testers' public IPs as required by the
implementation plan. ENet provides neither authentication nor encryption.
