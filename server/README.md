# Private battle lobby prototype

Build independently of the game, ROM, renderer, and game submodules:

```sh
cmake -S server -B build/lobby -DCMAKE_BUILD_TYPE=Release
cmake --build build/lobby -j 4
ctest --test-dir build/lobby --output-on-failure
build/lobby/bm64-lobby 127.0.0.1 6464 CLIENT_FINGERPRINT_HEX [CLIENT_FINGERPRINT_HEX ...]
```

ENet 1.3.18 is pinned to upstream commit
`2662c0de09e36f2a2030ccc2c528a3e4c9e8138a`. CMake fetches it during configuration.
Each fingerprint argument is 64 hexadecimal digits identifying one accepted client
build. The server compares the complete 32-byte value before allocating a seat.
Pass one fingerprint for a single-platform room, or up to four for a set of
platform builds validated together, such as one Mac and one Windows build made
from the same source. Duplicates are refused.
On macOS, obtain that value from the exact client bundle after selecting its ROM:

```sh
build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled --online-fingerprint
```

The command hashes the executable and stored ROM with SHA-256 and includes the
platform architecture and fixed battle profile. `APP_FOLDER_PATH` can select an
isolated directory containing `bm64_us.z64`. It exits without starting the game.
`python3 tests/run_fingerprint.py` checks the command against Python's SHA-256;
the fingerprint CTest also covers standard vectors and build/ROM changes. The executable
includes the statically linked recomp, patches, and runtime. This identifies the
build/profile; it does not certify cross-machine determinism or audit every setting.
The Mac and Windows launchers' **Online Battle** screen uses this fingerprint and rechecks
the executable/ROM before every match. Enter the server address, port, and name;
choose the local controls to use, then Ready. P1 can start once at least two humans
are ready. Empty seats are CPUs. The lobby stays connected while a fresh game
window runs the match; Cancel or Leave tears it down. A failed launch returns the
room to Lobby. Local input preferences are copied into disposable settings.
Existing transport fixtures continue to use their test token.

To admit a Mac/Windows pair, build both from the same commit and run
`python3 tests/run_online_transition_tests.py startup`, then `round`, then `match`
on each machine. Copy the Windows `experiments/results/{startup,round,match}-transition-a.txt`
logs to the Mac and run, for each phase:

```sh
python3 tests/run_online_transition_tests.py --cross-build PHASE MAC_LOG WINDOWS_LOG
```

Only if all three pass, run `python3 tests/run_fingerprint.py --rom PATH_TO_ROM`
on each machine. It checks the native fingerprint command and prints a
`FINGERPRINT:` line. Start the server with both values. Rebuilding either side
changes its fingerprint and requires validating the pair again.

This remains an early playtest. Audio plays on the first simulation of each
update; corrected replays are silent, and online rumble is disabled. Selected
controllers and calibration are preserved across match processes. Earlier builds
completed a human Mac/Windows internet match and scripted LAN rematches. Repeated
physical-controller sessions, wider hardware coverage, and the four-person remote
acceptance run remain open. Historical passes do not certify a rebuilt pair.

The current wire protocol is version 2 (private owner/game channel version 4).
Rebuild the relay and every client together, then revalidate their fingerprints.
Old version-1 peers are rejected. Every 300 game updates, the game hashes full RAM
and comparable registers at a confirmed boundary. The relay compares every
player's report and ends the match on disagreement, with the guest frame in the
error. A missing peer report times out after five seconds. Play continues while
the acknowledgment is in flight, but a second check waits for the first one.
`NETWORK_CHECK_COST` logs the hash and presentation-drain cost in milliseconds;
these checks are a diagnostic guard, not a proof that native continuations match.

The launcher's `online-match-logs` directory keeps the ten most recent attempts,
including across reconnects. Failure messages include the corresponding log path.
Windows match children start in the executable's directory so their assets do not
depend on the parent's working folder. Temporary match folders support Unicode.

The server owns one room and serializes membership, Ready, and Start. Roster
revisions change on membership changes and return to Lobby; Ready flag updates
keep the revision so simultaneous Ready requests for the same members remain
valid. A failed or disconnected player aborts loading/play for everyone. A
win continues through the award screens into another battle. Leaving or cancelling
returns everyone to the lobby, keeps connections, and clears Ready. A new lobby
start gets a new match ID. The start epoch is server monotonic milliseconds. The connection client
uses the lowest-RTT estimate among eight recent clock samples, requires at least
three fresh samples before acknowledging loading, and cancels a late start. Frame
numbers and bounded simulation lead still need the input protocol; clock estimates
alone cannot synchronize gameplay over asymmetric routes.

Control messages use reliable ordered channel 0, explicit big-endian fields,
protocol version 2, and a 1,199-byte maximum. Channels 1–4 carry separate unreliable
sequenced input streams, with 32 recent inputs per upload. There are eight transport slots (four
playable seats plus pending/rejected joins), a five-second handshake deadline,
120 incoming messages per connection per second, a 64-KiB incoming wait
limit, and a 128-command outgoing queue limit. Slow or invalid peers are removed.
Loading has a 30-second deadline; partial state/finish agreement and stalled input
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
version 4 queues at most 2,048 fixed 240-byte records, supports partial nonblocking
writes, and fails if that bound is exceeded. The owner drains at most 128 child
records per poll. These input records are separate from checkpoints. Windows uses a private named pipe and an owned child-process job; its lifecycle
tests run on Windows too.

`input_network` runs four production client connections through actual ENet input
channels. It independently delays, drops, and duplicates received relays at the
client delivery boundary, checks corrected state against an oracle, and verifies
confirmed completion and stalled-match timeout. This is not OS-level packet
shaping or a ROM rollback test. Raw connection tests reject slot impersonation
and conflicting replacements. See [input frame rules](INPUT_PROTOCOL.md).

No ROM, checkpoint, or native pointer is sent. Only input records and small
state digests cross the network. Finish-frame agreement remains available for
non-network lifecycle fixtures; live network games continue after a win.

After building the native game and this server's test targets, run
`python3 tests/run_network_game.py all` to compare four real ROM clients under
normal delivery, independent delay/loss, and a changed-input negative control.
It also changes just one client's replay and requires the live desync check to
close all four games and return their connections to the lobby.
`python3 tests/run_network_game.py match` completes a full first-to-three match
and enters a rematch with delayed delivery. These explicit macOS tests require a local ROM and open
four game processes. They hash full RAM at verification boundaries, use recorded
controller inputs through the actual relay/checkpoint path, with audio enabled;
they do not establish live-player feel or timing performance.
For a cross-machine scripted run, use one `network_peer` per machine with a
positive `STOP_FRAME`. Each peer reports its confirmed state; P1 cancels only
after the relay acknowledges that all peers reached the final checkpoint with
equal hashes. Every peer rejects an early cancellation. Zero is refused because
network matches no longer finish automatically after a win.

No dedicated datacenter deployment has been performed. For remote private tests, bind the chosen
interface and restrict its UDP port to testers' public IPs as required by the
implementation plan. ENet provides neither authentication nor encryption.
