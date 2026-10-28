# Shared benchmark hosts

Owner instruction, 2026-10-28: rotate between the workstation, `SERVER`
(`ssh server`) and Steam Deck (`ssh steamdeck-mph-wifi`, wired alias
`steamdeck-mph`). Select any reachable host with enough headroom. An exclusive
campaign reservation, quiet desktop or foreign lease is not an admission gate.
Preserve other agents' processes, lock files, services and workspaces.

`tools/cyc/host_headroom.py` takes a short read-only CPU/memory/storage sample.
Its light-game defaults require half a logical core of aggregate spare capacity,
one core below85% busy, 768MiB available memory and 1GiB workspace free. These
are capacity allowances rather than a low-noise benchmark requirement. Raise
storage/memory allowances for substantial builds and asset staging. If a host
lacks capacity, try another; do not terminate jobs or wait for a reservation.

Run matched baseline/candidate packages on the same host, renderer, feature
profile and workload. Record the probe and per-leg contention, retain raw
samples, and treat mixed small differences as neutral/inconclusive. Cross-host
timings are separate results. Warmups, setup and correctness captures remain
separate from ordinary timed gameplay. Unattended launches remain muted and
hidden; owner handoffs use normal windows/audio.

Steam Deck is a Linux target: qualify a native Linux build or an explicitly
recorded existing Wine/Proton installation before timing. Do not compare a
Windows reference with a native Linux candidate. Keep staged copies bounded
under the task workspace and remove disposable data after retaining evidence.
