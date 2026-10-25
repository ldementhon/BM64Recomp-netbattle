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
Release fingerprint generation and the game client connection are still pending;
the test value is only a fixture, not a compatibility certification.

The server owns one room and serializes membership, Ready, and Start. Roster
revisions change on membership changes and return to Lobby; Ready flag updates
keep the revision so simultaneous Ready requests for the same members remain
valid. A failed or disconnected player aborts loading/play for everyone. An
ordinary completed match keeps connections and clears Ready. Every match gets a
new ID. The start epoch is server monotonic milliseconds; client clock mapping
and late-start handling must be implemented before actual gameplay.

Control messages use reliable ordered channel 0, explicit big-endian fields,
protocol version 1, and a 1,199-byte maximum. Channels 1–4 are reserved for player
input streams and are currently rejected. There are eight transport slots (four
playable seats plus pending/rejected joins), a five-second handshake deadline,
120 incoming control messages per connection per second, a 64-KiB incoming wait
limit, and a 128-command outgoing queue limit. Slow or invalid peers are removed.
Loading has a 30-second deadline; partial finish agreement has five seconds.

The tests exercise actual loopback UDP connections for 2/3/4 clients, simultaneous
joins, full-room and incompatible-build rejection, stale readiness, repeated
start/finish, load failure, and loading disconnect. Pure room tests additionally
cover timeout, digest mismatch, stale matches, P1 permissions, and playing
disconnect. Test programs keep assertions enabled in Release builds.

This is not yet a playable client or input relay. No ROM, checkpoint, or native
pointer is sent. Match completion currently checks client frame agreement; it
must also require the input protocol's confirmed frontier before live use.
No deployment has been performed. For remote private tests, bind the chosen
interface and restrict its UDP port to testers' public IPs as required by the
implementation plan. ENet provides neither authentication nor encryption.
