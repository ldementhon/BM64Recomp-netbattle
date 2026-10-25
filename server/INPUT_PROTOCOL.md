# Input frames and retained history

The network frame counter is relative to the acknowledged Player Select state.
Boundary F is the checkpoint before consuming input F. Advancing it produces
boundary F+1. Inputs 0 and 1 are neutral. Sampling a physical controller at F
creates input F+2 exactly once; retransmissions, repeated controller reads, and
replay cannot sample it again. Guest completion at G maps to boundary G minus
the acknowledged initial guest frame.

`RollbackInputs` holds actual received inputs outside checkpoints. A simulation
records the pads it used for each frame. New differing actual input requests the
earliest affected checkpoint; the caller restores and replays every update up to
the unchanged head. It must service new packets during replay and honor another
earlier correction before advancing normally. CPU pads are neutral at this layer;
the game simulates the CPUs. Human input is identified by the server-assigned slot.

Each receiver reports an exclusive contiguous frontier for each human stream.
The relay's confirmed frontier C is the minimum across all human receivers and
streams. Server receipt by itself cannot advance C. A client can consume frame F
only while F < C+12, leaving head at most C+12. Sampling at that stalled head can
produce input C+14. Uploads retain 32 recent inputs and repeat while stalled. Every
valid repeat is relayed again, repairing loss on the server-to-client leg.

At local head H, this client's own sampled-input frontier is at most H+3, so the
global confirmed frontier cannot exceed H+3. The furthest permitted remote input
is therefore H+17. Retaining inputs from H-13 preserves every possible correction
and the preceding held value; the inclusive window H-13 through H+17 uses 31 of
32 slots. Inserts reject any overwrite of a protected entry. Corrections are at
most 12 frames deep, within the 16 checkpoint slots. These bounds include the
two-update sampling offset. Do not increase them independently.

Input packets have magic `BM6I`, a big-endian protocol version, a direction byte
(upload/relay), match ID, slot, first frame, and count (0–32). Uploads carry four
contiguous receipt frontiers; inactive CPU entries are zero. Relays carry C. Each
pad is a 16-bit N64 button mask and two signed bytes (-127…127); reserved button
bits are rejected. Maximum upload size is 185 bytes, below the 1,199-byte cap.
The transport channel is 1+slot, separate from reliable control channel 0.

This document describes the implemented codec/history model. Live ENet delivery,
five-second stall termination, and the game's checkpoint adapter are not yet
wired to it. `input_history` exercises a small deterministic simulation oracle;
it is not evidence that ROM rollback is integrated.
