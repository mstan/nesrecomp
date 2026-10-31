# NES25 callback-owned PCM queue package

Reject the delivered package after the paced SMB3 screen. It has no established
performance benefit on this setup. The selected NES20 runtime and current audio
path stay unchanged. This is not a universal callback-versus-queue verdict.

The prototype owns a bounded 32 KB PCM ring and lets SDL's callback consume it.
Producer writes, queries and clears use the SDL device lock; the callback does
no allocation and pads shortages with signed-16 silence. The existing ~100 ms
admission threshold and a complete 4096-sample producer chunk fit in capacity.
Menu, file-load and rewind clearing follow the same ownership interface. Exotic
rates keep SDL's original queue; a failed callback open retries that queue.
Emulation, mixing, volume and pacing stay in their original places.

The existing console audio executable passes 3,481 checks, including full and
wrapped FIFO order, partial consumption, empty silence, clearing and maximum
producer-chunk capacity. Both built variants also match all 28 headless output
hashes and their 600-frame current-build continuations.

The actual performance invocation uses the existing run_sdl_interval helper on
normal paced, hidden software-SDL gameplay, requests the known WASAPI backend,
and preserves enabled synthesis/output with volume zero and rewind 100/4.
Setup/load is excluded. Each leg resumes frames 2401 through 3601 and takes
about 20.11 seconds. Whole-process CPU includes callback threads. Paired CPU
seconds are 4.015625/3.812500, 3.906250/3.968750 and 3.640625/3.953125
(reference/candidate). One improves and two worsen; the raw median changes
3.906250 to 3.953125 seconds. The apparent 1.20% increase is not a precise
reproducible regression or a numerical retention floor. No useful gain is
established, so no expanded matrix or extra telemetry is warranted.

Callback activation after the possible queue retry is not separately observed.
No queue-occupancy, host-PCM, audible-underrun or physical-latency result is
claimed. VSync is requested off, while initial state readback says active; do
not label active VSync-off verified. These limits narrow the conclusion to the
actual delivered package rather than claiming its callback branch universally
rejected. No instrumentation was added merely to salvage this neutral result.

The saved run.py is the earlier 600-frame dummy/uncapped template used only up
to its correctness/continuation prefix. The separate inline paced invocation
calls the existing helper with the full actual argv, settings, affinity and
1200-frame interval retained in performance.json. Raw evidence is at
G:/Temp/Codex/nes110-audio-candidate-20261031/smb3; source/compile identities and
test logs remain in the campaign workspace. See the
[machine-readable result](2026-10-31-audio-queue-candidate-results.json).
No implementation PR, main/master merge or release. Continue to NES26 after
independent durable handoff; other audio ownership designs remain possible.
