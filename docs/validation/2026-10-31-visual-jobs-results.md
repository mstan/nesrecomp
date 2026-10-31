# NES28: reject the immutable row-worker prototype

Keep the selected serial voxel renderer. A private one-worker design reads the
existing owned geometry/texture plan and partitions output into separate rows,
preserving per-pixel primitive/depth/overlay order. Game callbacks and machine
state remain on the caller, with completion before plan swap, actors and loads.
This treats ownership as replaceable rather than a permanent threading veto.

Current selected-build diagnosis finds299renders/54replays/245cachehits:
total264.307ms, owned-plan38.213ms, replay166.815ms (~3.09ms/miss), actors7.238ms.
Three actual camera key events are included in that diagnostic. These are
inclusive wall scopes with clock/scheduling overhead, not removable CPU bounds.
The control reproduces the selected voxel object's runtime sections;122other
objects remain unchanged. Old pre-retention triangle costs were not reused as
current cost. Two prelaunch staging mistakes were corrected without runtime edits.

The matched **fixed native route**, without those diagnostic camera key events,
uses diorama300frames, normal UI compiled, software SDL1280x720, dummy audio,
rewind100/4 and the same two physical cores LP12+14. Setup and warmups excluded.
Three pairs give CPU0.859375/0.984375,0.875000/0.953125,0.859375/0.984375seconds.
Median CPU rises **14.545%**, all three worse. Median complete interval wall
rises1.0031724 to1.0784738seconds (**7.506%**), all three worse. Process-summed
CPU includes worker work. Median process peak working set is103,886,848bytes
both; peak private commit167,550,976 to167,665,664bytes, including setup.

All22native frame/hardware/RAM/PCM/native and presented picture hashes match.
No invocation-count, race-tool, resource-failure, first-person/animated or
portable runtime qualification is claimed; further checks are unnecessary for
this rejected candidate. Windows-only worker APIs retain gated serial fallback.
No per-frame latency distribution or physical input/display claim is made.

The private prototype remains unselected; no runtime PR. A different projected
command/binning design or moving/animated voxel/HD workload remains an explicit
residual, not an exhausted architecture. Final fresh CLI/five-game combined
builds and owner approval remain separate. See [raw samples and identities](2026-10-31-visual-jobs-results.json).
