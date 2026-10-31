# NES25 real-device audio host attribution

Defer host buffering/queue consolidation for the measured SMB3 configuration.
An actual opened WASAPI device and normal paced gameplay show little work in
the existing host drain/mix/volume/queue loop. No runtime change or performance
gain is selected. This is a scoped prioritization decision, not a gain floor
or a claim that all audio ownership opportunities are exhausted.

| Observation | Measured value |
| --- | --- |
| Device | WASAPI, 48 kHz, signed16 native-endian, mono, 1024 samples |
| Audio settings | Emulation/output enabled; volume 0; device playing |
| Normal measured loops | 1,139, frames 2462–3600 |
| Host drain/mix/volume/queue elapsed | 2.0911 ms total |
| Samples produced / submitted | 909,702 / 909,702 |
| Software queue bytes | min 0, max 3,540, mean 2,024.571 |
| Empty software-queue observations | 3; not proof of audible underruns |
| Whole resumed interval | 20.083920 s wall, 3.953125 s process CPU |
| Oldest main thread CPU | 3.9375 s |
| Other enumerated worker CPU | 0.015625 s, one accounting tick |

The current selected post-NES08 SMB3 package runs hidden with software SDL at
1280x720 with stock 256x240 content, VSync off, rewind 100/4 and a fixed gameplay route. The first 60
resumed frames and final frame are outside the audio scope. The actual device
reports enabled synthesis/output and playing status; this is not dummy-device
timing. Volume zero mutes delivery while drain and mixing still run. Its
volume-scaling cost is included; volume 100 uses a different host branch.

The outer elapsed scope includes its queue query, instrumentation and
preemption; it is not exclusive CPU or removable savings. Guest APU synthesis
and the SDL backend thread are outside it. All enumerated owned thread handles
were retained through process exit for CPU accounting. Names are unavailable,
so the audio thread is not individually identified. External Windows audio
service/hardware work and later-born threads are not separately attributed.
Observed thread sums match process CPU at 15.625 ms accounting granularity;
zero ticks are not proof of zero work.

Produced equals attempted queue submissions in the measured scope. SDL_QueueAudio return values were not checked, so successful submission was not independently verified. SDL's software queue can empty
while driver/hardware buffers still contain audio. Neither the three empty
observations nor software bytes establish audible glitches or physical
latency. Other devices/titles and low-power-host audio service costs remain
explicit residual opportunities.

The diagnostic uses the actual selected SDL object's frozen compile recipe.
An unchanged control reproduces every present runtime COFF section. Only the
instrumented SDL object differs; the remaining selected package stays intact.
No optimization, behavior refactor, runtime PR or owner playback handoff is
proposed. Final supported CLI/five-title builds and combined checks remain
separate. See [raw identities and results](2026-10-31-audio-host-results.json).
