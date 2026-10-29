# Presentation bridge attribution: consumer job first

NES15, `beads-2dw.1.110`, campaign `beads-2wy8.1`, selected runtime through
`55644d2`, documentation base `272b8b2`. Measured deferral of a standalone compatibility-view rewrite, independently
validated against raw scopes, source, profiles, identities and reservations.
No runtime optimization or A/B speedup is claimed.

| Active Zelda profile | Bridge calls | Bridge QPC total | Frame-end preparations | Present callbacks | Larger consumer scope |
|---|---:|---:|---:|---:|---|
| Seamless Fit | 972 | 11.5522 ms | 673 | 374 | world preparation: 278.0401 ms |
| Remastered | 598 | 8.7925 ms | 299 | 0 | HD sampler is outside these consumer scopes |
| Diorama | 972 | 13.0560 ms | 673 | 374 | voxel preparation: 2387.3094 ms |

One diagnostic run per profile on this PC, capacity reserved, actual supported
game code and independently prepared current states. All have 299 admitted
input callbacks. The instrumentation admits `cyc_ring_frame`1802..2100, after
paused state1801 setup; ordinary SDL resumes1801..2101. Video uses SDL's dummy
driver with software1280x720 rendering, and audio is emulated/mixed/queued to
dummy output. Initial native images show Link in active gameplay. The diorama
option is enabled in both preparations and SDL, with actual voxel work logged;
the HD logs confirm the real enabled Remastered pack and512x480 picture.

These are instrumented QPC wall scopes, including scheduling/cache effects and
clock overhead. They are neither exclusive process-CPU attribution nor a
recoverable-time bound. Do not divide by process CPU to forecast a gain or add
nested bridge/consumer scopes together. No physical-display/GPU timing claim.

The compatibility adapter copies OAM/nametable/palette and reads all 8192 mapped
CHR bytes on every sync. CHR reads dominate its measured scope. Zelda calls it
from input and frame-end preparation; its non-HD present callback also repeats
frame-end preparation. The actual diorama preparation is much larger than
the bridge, and it runs through both callback paths. Prefer one prepared
consumer render job under NES11 before introducing another standalone view
interface. This is a measured priority decision, without a gain floor, idle
host requirement or assumption that old internal machinery must be preserved.
A later full render job may subsume the copy work or justify reopening NES15.

The first attempt omitted explicit `--mods-root` during headless preparation.
Its HD state failed program identity against the correctly patched SDL process;
that entire attempt is excluded. The corrected run supplies the same explicit
root on both paths, separately prepares each profile, and passes state loading.
`--present-out` did not create a final window PNG; no final image verification
is claimed. Missing HD art/unsupported rules remain shared limitations.

Scope correction: NES15 is the legacy OAM/nametable/palette/CHR bridge.
The full `HdPixel` adaptation belongs to a possible NES13 follow-up. The prior
NES13 narrative linked that adaptation to NES15 incorrectly; the register's
NES15 definition was correct.

[Machine-readable results](2026-10-28-presentation-bridge-results.json) retain
raw scopes, source/binary/object identities, commands, settings and capacity
telemetry. The verified raw archive and diagnostic sources remain under
`G:/Temp/Codex/nesrecomp/pipeline-burndown-20261028`. No implementation PR is
warranted for this deferral. The fresh validator confirmed the evidence and requires its final durable
Beads/commit check before authorizing NES11. Final combined owner builds
and approval remain pending; FDS stays parked.
