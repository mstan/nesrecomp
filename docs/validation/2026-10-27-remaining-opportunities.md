# Remaining NES native performance opportunities

Tracking: `beads-2dw.1.109`, related to `beads-2dw.1.101` and optimization
campaign `beads-2wy8.1`. Isolated branch:
`docs/remaining-native-opportunities-20261027`.

Current execution: `beads-2dw.1.110`, branch
`perf/pipeline-burndown-20261028`. The source/evidence audit below is historical.
Selected implementations now include the native CPU/device interval service
(NES02/03/05), sprite-output spans (NES01 stage), and base-APU event spans
(NES06 stage). Five stock games have matched output checks and raw A/B samples
on both hosts. See [board/deadline results](2026-10-28-board-deadline-results.md),
[sprite results](2026-10-28-sprite-span-results.md) and
[APU results](2026-10-28-apu-event-results.md). Zelda's enabled HD lookup and
composition job is now qualified in its own module; other games keep the
original HD runtime. See [HD results](2026-10-28-hd-plan-results.md), including
the rejected universal packages and SMB's unchanged-runtime proof. Zelda's
fixed SNROM SRAM code now joins the native deadline service; see
[SRAM results](2026-10-28-sram-native-results.md). SERVER Fit improves5.128%
and Remastered1.442%; stock is neutral/inconclusive. Nineteen items now have
qualified evaluation decisions; NES01 is qualified for whole stable HLE raster
jobs and NES29 remains parked.
Guarded native ROM is now evaluated under NES09; see
[guarded ROM results](2026-10-28-guarded-rom-results.md).
Whole stable HLE raster jobs are now qualified across the five stock titles
and Zelda Fit/HD profiles; see [raster results](2026-10-28-raster-job-results.md).
Zelda isolated native execution is selected under NES18; NES20 snapshot
replacement is deferred after measured attribution. See
[isolated service results](2026-10-28-isolated-native-results.md).
Remaining evaluations and the final combined feature handoff are still open. FDS remains
parked. No main/master merge or new release is authorized before owner review.

Source checkpoint: engine `957e9902de9b91107e61bc981f2ca871c8740446`;
Zelda `5d90eb7a10e13f5dd5c693a0204b29f7917ec198`. This is a source and retained
evidence audit. No optimization implementation, game launch, stress testing or
new speedup measurement was performed. The entries are credible remaining
hypotheses, not promised savings or proof that every possible opportunity has
been discovered. Existing deferred/rejected prototypes are evidence to reuse.

## Did validation and artificial boundaries limit the prior work?

**Partly, but the NES result was not a menu-only benchmark.** The accepted
[October 27 package](2026-10-27-native-opportunities.md) measured ordinary SDL
gameplay frames 1801–3000, with audio emulated/mixed/queued, five alternating
A/B pairs, excluded setup and separate correctness observers. Stock workload
CPU reductions were Metroid 13.94%, Zelda 11.25%, Gumshoe 12.88% for its combined
package and SMB1 2.74%. SMB3 remained neutral/inconclusive. Different packages
and old LLE baselines must not be presented as today's common baseline.

The limitations are documented:

1. The [October 22 investigation](2026-10-22-cycle-performance.md) describes
   nineteen prototypes, eighteen admitted legs per isolated timing case and
   a final queue stopped following owner steering that testing had become
   excessive. That is evidence of validation expanding beyond a proportionate
   delivery loop. Quiet-host waits themselves were appropriate coordination,
   not a reason to kill another session's work.
2. The [earlier HLE scope](../NES_ENGINE_HLE.md) explicitly retained the clock,
   DMA/APU and mapper adapters and left larger scanline/event work for later.
   The delivered native service still runs individual dot/half-dot work and
   starts/finishes ordinary CPU cycles separately. This was a bounded
   implementation, not consolidation of the complete execution/render path.
3. The prior burndown evaluated **a generic pixel bypass**, then preserved raw
   pixels, status, mapper and Zapper work. Those are legitimate obligations.
   They do not require the old producer machinery: a replacement can generate
   the required products/events once and feed stock/custom/HD consumers.
   No retained evidence establishes that this whole-path alternative was tried
   and exhausted.
4. “RAM is already direct” and “ROM reads already fold” establish useful local
   facts, not that generated CPU, memory, dispatch and scheduling jointly have
   no removable work. Similarly, deferred SRAM views, core-only IPO and
   optional enhanced-mode timing are unfinished investigations, not negative
   performance findings.
5. The active performance guide contained a stale 3% retention floor and a
   blanket zero-dispatch-miss gate. This audit corrects them to the owner's
   per-game neutral/positive and shared-defect rules. The accepted SMB1 2.74%
   result shows that the old floor was not enforced against that delivery;
   the stale text was a future trap, not evidence the accepted package was
   improperly rejected. Historical timing percentages stay unchanged.

Preserve console-visible timing, bus/device behavior, gameplay, audio, native
saves and current-build restore/replay. Do not impose identical private
structs, snapshots from old builds, callback counts, executing a reference
machine inside HLE, or the existing subsystem ownership boundaries as additional
requirements. Diagnostic representation adapters can serve observers without
maintaining duplicate simulation. Unsupported cases retain a functioning
faithful/portable path.

## Priority and the next coherent implementation

P1 means a substantial service/pipeline replacement worth resolving first;
P2 is narrower or depends on attribution/ownership; P3 is conditional/lower
priority. These are architectural hypotheses, not percentages. Many overlap:
do not add their possible gains together or start thirty-one experiments.

The first stock-game goal should be **one native interval execution/render
service**: own a stable run of CPU work and the PPU/APU/mapper deadlines and
products together (NES01–03 and NES06). Start with a bounded inclusive profile
of the already-delivered HLE SMB3 active-level route and SMB1 as a control.
Reuse the existing routes, fixtures and negative receipts. If the current
inclusive cost selects a different large job, choose that job before coding.
The old approximately 68–70% LLE PPU self-sample share cannot bound or predict
post-HLE savings.

The next clearly source-backed complete jobs are the HD recording/sampling/
bridge pipeline (NES13–15), and Zelda's persistent neighboring-room service
with isolation ownership (NES18/20). Active HD shifts tile metadata and records
per-pixel copies; each Zelda neighbor tick takes/restores a machine snapshot.
Those mechanisms are visible in source, but their recoverable CPU share is
unmeasured. Zelda already caches room rasters and limits ordinary simulation
to the view plus adjacent rooms; do not claim those existing features anew.

## Burndown

Checked items have a measured evaluation/selection decision for this campaign's
qualified scope; they do not claim architectural exhaustion. Final combined
build/feature and owner checks remain separate. Every unchecked item still
needs a decision or remains explicitly parked. A local implementation or
passing fixture alone does not complete an evaluation. The machine-readable
companion records evidence, status, next check
and behavior contract for each stable ID:
[remaining opportunities JSON](2026-10-27-remaining-opportunities.json).

Execution update, October28: the native CPU/memory/device interval package
advancing NES02/NES03/NES05 is now stock-qualified across all five titles on
both hosts. The same engine package includes NROM, MMC1, MMC3 and GxROM;
current per-game measurements are in the
[board-deadline results](2026-10-28-board-deadline-results.md). Enhancement
profiles and the other opportunity evaluations remain open. This does not
close the per-pixel raster or APU-event algorithm work under NES01/NES06.


### Native execution and machine intervals


- [x] **NES01 · P1 · Whole raster spans instead of one dot at a time**

  Stable HLE tile jobs now own background fetch, pixel composition/output, OAM pairs and observable events across complete eight-dot intervals. Boundary adapters and active HD/other-board paths remain; the qualified scope has a selection decision.

  Scope: SMB3 active MMC3 gameplay; SMB1 NROM control; subsequent affected cycle titles.

  Decision: Selected whole stable HLE tile jobs after matched five-title stock and Zelda Fit/HD checks. See [raster results](2026-10-28-raster-job-results.md). Final combined regeneration and owner handoff remain; active HD/other boards retain their working paths.

  Preserve: Correct pixels/opacity, sprite-zero and overflow publication, register effects, mapper address/IRQ events, Zapper light and current-build continuation. LLE remains a separate working reference.

  Evidence: [runner/cyc/hw_ppu.c:1273](../../runner/cyc/hw_ppu.c#L1273), [runner/cyc/hw_ppu.c:1306](../../runner/cyc/hw_ppu.c#L1306).


- [x] **NES02 · P1 · One CPU/device deadline service across multiple CPU cycles**

  Generated CPU accesses still enter start/read-or-write/finish coordination. The existing optimization covers a tail within one CPU cycle. Consolidate ordinary execution and PPU/APU/mapper progress until the next actual bus/device/interrupt/host deadline.

  Scope: Current cycle games; NTSC first, PAL when selected.

  Next: Measure inclusive crossings in the same gameplay interval as NES01; implement a deadline-owned service rather than another callback-only fast path.

  Preserve: NMI edge and IRQ poll ordering, DMA readiness, open bus, mapper clocks, observable accesses and hook/stop deadlines. An unchanged number of helper calls is not a contract.

  Evidence: [runner/cyc/hw_machine.c:177](../../runner/cyc/hw_machine.c#L177), [runner/cyc/hw_machine.c:62](../../runner/cyc/hw_machine.c#L62), [runner/cyc/cpu6502.h:83](../../runner/cyc/cpu6502.h#L83).


- [x] **NES03 · P1 · Integrated generated CPU blocks and ordinary memory service**

  Static targets already use native gotos and ROM constants; RAM is already direct inside the bus router. Remaining work is cross-unit state traffic, instruction/chunk boundaries and repeated clock/bus guards. Revisit block/register ownership and observer-aware code generation as a coherent package.

  Scope: Current native cycle games and active mod/hook configurations.

  Next: Inspect one hot emitted block without editing it; attribute inclusive CPU/bus work after HLE and compare a regenerated fused-block experiment, not native coverage percentages.

  Preserve: Interrupt boundaries, mapper writes and slot changes, hooks/read substitutions, self-modifying code, DMA and portable fallback. Fix the generator, never emitted C.

  Evidence: [recompiler/src/cyc_codegen.c:76](../../recompiler/src/cyc_codegen.c#L76), [runner/cyc/cpu6502.h:92](../../runner/cyc/cpu6502.h#L92), [runner/cyc/cyc_run.c:79](../../runner/cyc/cyc_run.c#L79).


- [x] **NES04 · P2 · Guarded native cartridge SRAM views**

  Selected evaluation: engine111b264 and Zelda b602d26 qualify declared fixed
  SNROM copies with physical-byte validation, CPU/mod/committed-write watches,
  both A12 read gates and the device deadline cursor. HLE/LLE tests pass all
  four alignments, including untraced cursor use. Stock/Fit/HD outputs and
  Fit room/actor records match; SERVER Fit+5.128%, HD+1.442%, stock neutral.
  [Evidence and limits](2026-10-28-sram-native-results.md). Other mappings
  retain fallback. The paragraph below is the original audit hypothesis.

  The RAM-view physical mapping admits CPU RAM and FDS linear PRG RAM, not general cartridge RAM. Zelda copied routines remain a concrete candidate; a mapping-generation and write-watch contract can make them native without pretending SRAM is immutable.

  Scope: Zelda copied MMC1 SRAM code; other supported writable-code mappings conditionally.

  Next: Reuse beads-2dw.1.105; first attribute host CPU for the copied routines. Then qualify visible mapping, byte writes/debug pokes and PPU-edge read-gate changes on the supported MMC1 board.

  Preserve: Every folded byte remains the visible byte, mapping/write changes exit or invalidate, restore revalidates and unsupported mappings interpret. Earlier 10.1% is emulated-cycle share, not possible CPU savings.

  Evidence: [runner/cyc/cyc_ramview.c:19](../../runner/cyc/cyc_ramview.c#L19), [runner/cyc/cyc_ramview.c:30](../../runner/cyc/cyc_ramview.c#L30). Reuse `beads-2dw.1.105`.


- [x] **NES05 · P2 · Board and observer capabilities chosen at stable boundaries**

  MMC1 map stability and inert read guards are already delivered. Remaining repeated ROM-snoop, board, cheat/data-overlay and device-observer decisions can be consolidated into a qualified service profile with invalidation when configuration changes.

  Scope: Boards/mod configurations with active versus absent read snoops, overlays and IRQ services.

  Next: Count and inclusively sample the active callbacks in the selected gameplay interval; only build a profile when it removes a material complete path.

  Preserve: Bank-qualified cheats, data overlays, MMC5 read snoops, actual active board devices and configuration changes. Do not silently specialize away supported mods.

  Evidence: [runner/cyc/hw_machine.c:227](../../runner/cyc/hw_machine.c#L227), [runner/cyc/hw_mapper.c:1301](../../runner/cyc/hw_mapper.c#L1301).


- [x] **NES06 · P1 · Complete event-driven APU synthesis spans**

  The accepted cache avoids recalculating base-channel level, but timers, sequencer/DMC checks, expansion sampling and accumulation still advance every CPU cycle. Give the next channel/register/DMC/sample event one owner and synthesize the interval.

  Scope: Audio-on cycle games; base APU first, expansion devices separately.

  Next: Use audio-on gameplay after the current cache, recording timer-event density and inclusive audio cost. Replace a whole interval, with rounding/accumulation handled deliberately.

  Preserve: PCM, phase, output filters, envelopes, frame IRQ, DMC bus steals and controller delays. Exact old floating-point accumulation may require a matching arithmetic formulation or conservative fallback.

  Evidence: [runner/cyc/hw_apu.c:350](../../runner/cyc/hw_apu.c#L350), [runner/cyc/hw_apu.c:601](../../runner/cyc/hw_apu.c#L601).


- [ ] **NES07 · P2 · Native DMA transfer service with exact device deadlines**

  OAM and DMC DMA still traverse the ordinary cycle machinery. Consolidate safe transfer runs and device advancement rather than substituting an unqualified memcpy.

  Scope: Actual OAM/DMC-heavy gameplay; controller/Zapper interactions when affected.

  Next: Attribute a real DMA-heavy interval; reuse collision fixtures and test a transfer service only if the post-HLE share warrants it.

  Preserve: Stolen cycles, CPU-held address, controller/register/open-bus reads, DMC/OAM collisions, interrupts and mapper observations.

  Evidence: [runner/cyc/hw_machine.c:197](../../runner/cyc/hw_machine.c#L197), [runner/cyc/hw_apu.c:601](../../runner/cyc/hw_apu.c#L601).


- [ ] **NES08 · P2 · Qualified polling-loop and blank-interval fast forwarding**

  Native code can still execute an expensive guest wait. A verified loop can advance to its first observable event while accounting for repeated reads and elapsed device work; rendering-off intervals are another opportunity.

  Scope: Content-qualified active-gameplay waits and rendering-off intervals.

  Next: Identify the exact active wait on gameplay traces. Reject menu/boot-only counts as gameplay attribution; prove a complete loop service on one content-qualified loop.

  Preserve: Final registers/RAM, loop exit, side-effectful reads, interrupt sampling, device time and input latency. No arbitrary dropped cycles.

  Evidence: [runner/cyc/cpu6502.h:78](../../runner/cyc/cpu6502.h#L78), [runner/cyc/hw_machine.c:57](../../runner/cyc/hw_machine.c#L57).


### Native content and whole game services


- [x] **NES09 · P2 · Remaining hot ROM and RAM execution coverage**

  Selected evaluation: engine `e358f81` / Zelda `cf52e97` admit exact-byte
  guarded native ROM under atomic patches, using separate ordinary/checked
  dispatch families. The selected run has Fit +4.902% (four faster, one tie),
  stock neutral, and mixed HD +2.813% (inconclusive). All outputs and current-
  build replay match; other targets retain default-off code. No five-of-five
  win rule is imposed. [Evidence and limits](2026-10-28-guarded-rom-results.md).
  Further missed-code investigation is conditional on demonstrated hot cost.
  The paragraph below is the original audit hypothesis.

  Gumshoe received 2635 seeds in its accepted combined package. Other routes, titles, overlays and RAM images remain candidates only where meaningful hot execution still interprets.

  Scope: Routes/titles with observed hot ROM or RAM fallback; Gumshoe existing seed work is baseline.

  Next: Reuse bounded miss/capture logs from actual affected gameplay. Regenerate through the supported pipeline and measure the game; already near-100%-native SMB routes are not coverage projects.

  Preserve: Reachable content/byte identities, writes, fallback behavior and normal game progression; no percentage gain from a coverage counter alone.

  Evidence: [runner/cyc/cyc_run.c:79](../../runner/cyc/cyc_run.c#L79), [runner/cyc/cyc_ramview.c:207](../../runner/cyc/cyc_ramview.c#L207).


- [ ] **NES10 · P2 · Whole game-service native replacements**

  Trusted hooks provide a boundary for content-qualified native decompression, tile streaming, collision or entity update services. Translating the old instruction algorithm is not the only allowed implementation.

  Scope: A title with a demonstrated expensive decompression/streaming/collision/entity service.

  Next: Choose a repeatedly expensive service from an actual title profile, verify the ROM/disassembly contract, and replace the complete job rather than selected opcodes.

  Preserve: Inputs/outputs, shared RAM, RNG, relevant timing/device effects and failure paths; unsupported content keeps the original path. No guessed game logic.

  Evidence: [runner/cyc/cyc_hooks.c:87](../../runner/cyc/cyc_hooks.c#L87), [runner/cyc/cyc_mod.c:420](../../runner/cyc/cyc_mod.c#L420).


### One visual product pipeline


- [x] **NES11 · P1 · One stock/custom rendering product pipeline**

  A wide/custom view is composed after stock products exist. The old pixel-bypass rejection protected real observations, but did not evaluate a replacement that computes required status/mapper/light/pixel products once and feeds the final view directly.

  Scope: SMB1/SMB3/Metroid custom wide views; Zelda custom views; Zapper-aware consumers.

  Evaluation: One owned prepared Zelda picture is selected at `60a6821`,
  [PR11](https://github.com/mstan/LegendOfZeldaNESRecomp/pull/11), unmerged.
  SERVER stock4.18% and diorama38.77% CPU reductions each have five faster pairs.
  Fit and HD remain mixed/inconclusive, including negative medians. See
  [prepared picture results](2026-10-28-prepared-picture-results.md).
  The broader stock/custom raster-product fusion remains unmeasured; this is a
  scoped evaluated decision, not architectural exhaustion. Other titles are
  unchanged. Reassess fusion after remaining decoder/span/material evaluations.

  Preserve: Required screenshots/raw pixels, collision/status consumers, mid-frame effects, Zapper and mapper events. Avoiding duplicate work does not mean dropping those products.

  Evidence: [runner/cyc/hw_ppu.c:752](../../runner/cyc/hw_ppu.c#L752), [runner/cyc/hw_ppu.c:839](../../runner/cyc/hw_ppu.c#L839), [runner/cyc/cyc_render.c:163](../../runner/cyc/cyc_render.c#L163).


- [ ] **NES12 · P2 · Shared CHR/nametable/OAM preparation by mutation generation**

  Custom render helpers repeatedly decode rows and colors. Build reusable tile/row and sprite plans keyed to actual CHR, bank, nametable, palette and OAM revisions; share them with wide/HD/voxel consumers where contracts agree.

  Scope: Custom wide, HD and voxel consumers with repeated tile/sprite preparation.

  Next: Measure repeated decode/preparation in the chosen full-view job; reuse previous CHR-shortcut prototypes only after auditing their invalidation and unqualified timing status.

  Preserve: CHR RAM writes/bank changes, palette/scanline state, flips, clipping, transparency, sprite order and restore invalidation.

  Evidence: [runner/cyc/cyc_render.c:100](../../runner/cyc/cyc_render.c#L100), [runner/cyc/cyc_render.c:122](../../runner/cyc/cyc_render.c#L122).


- [x] **NES13 · P1 · HD recording as compact tile/run records**

  Active HD copies fetched-tile structures into eight lanes, shifts sixteen entries per half-dot and copies tile keys/palettes into per-pixel records. Replace repeated metadata movement with immutable tile identities, indices and run/event references.

  Scope: Zelda Remastered active HD; other real enabled HD packs.

  Decision: Both pooled compact pixels and circular delay records are deferred
  after two neutral/inconclusive actual Remastered A/B runs. No runtime change
  is selected. See [HD recorder results](2026-10-28-hd-record-results.md).
  The independent validator confirmed the deferral and durable-record handoff.
  NES15's separate bridge evaluation follows below. This is a decision, not exhaustion.

  Preserve: Bytes actually fetched across mid-tile bank/CHR changes, palette timing, sprite priority, masks/emphasis, exact native fallback and current-build saves.

  Evidence: [runner/cyc/cyc_hdpack.c:77](../../runner/cyc/cyc_hdpack.c#L77), [runner/cyc/cyc_hdpack.c:81](../../runner/cyc/cyc_hdpack.c#L81), [runner/cyc/cyc_hdpack.c:98](../../runner/cyc/cyc_hdpack.c#L98).


- [x] **NES14 · P1 · HD lookup, conditions and composition as one render job**

  A full recorded frame is adapted into shared pixel descriptors and then sampled/upscaled. Consolidate tile-key lookup, condition dependency evaluation, background layers and output runs; GPU-resident pack composition is a possible backend, not a requirement.

  Scope: Same real active HD-pack gameplay as NES13.

  Next: Use the same real-pack interval as NES13. Attribute recording/condition/lookup/blend costs and compare one complete render job with the existing sampler.

  Preserve: All supported condition semantics, sprite/background blending and fallback pixels; do not claim unsupported pack audio or conditions become implemented.

  Evidence: [runner/cyc/cyc_hdpack.c:133](../../runner/cyc/cyc_hdpack.c#L133), [runner/src/hdpack.c:237](../../runner/src/hdpack.c#L237), [runner/src/hdpack.c:1124](../../runner/src/hdpack.c#L1124).


- [x] **NES15 · P1 · Replace the copy-based presentation compatibility bridge**

  The bridge copies OAM/nametable/palette and reads 8192 CHR bytes into legacy-shaped globals. Expose one immutable frame/mapping view or revision-aware update service to consumers instead of rebuilding all compatibility products at every sync.

  Scope: Cycle custom/HD/voxel callers of cyc_presentation_sync.

  Decision: Measured deferral after one active Fit/HD/diorama attribution.
  The bridge is a small scoped cost beside repeated whole consumer preparation;
  prioritize one prepared render job under NES11. No A/B speedup or runtime
  change is claimed. See [bridge results](2026-10-28-presentation-bridge-results.md).
  The fresh validator confirmed this evidence; its durable-record handoff is next.

  Preserve: Consumers see the correct completed-frame/mapping data, live bank/CHR changes and state restores; keep an adapter for unported consumers.

  Evidence: [runner/cyc/cyc_presentation.c:16](../../runner/cyc/cyc_presentation.c#L16).


- [x] **NES16 · P2 · Whole-view widescreen preparation and span composition**

  The canvas is cleared, margins reconstructed and sprites decoded/composited on each new frame. Use one view plan, clipped spans and explicit dirty products rather than separate native-center and margin work.

  Scope: SMB1/SMB3/Metroid/Gumshoe real widened gameplay and Fit resizing.

  Evaluation: Metroid two-dimensional tile source/attribute plans v1/v2
  deferred after actual moving21:9/32:9 matched built-game runs showed no
  demonstrated CPU gain. Source/fixtures/actual adaptive pictures and current
  loads pass; operation-count reduction is not performance. See
  [wide-view results](2026-10-31-wide-view-results.md). Other three title
  runtimes are unchanged/unmeasured; broader producer alternatives remain
  an explicit deferred residual, not architectural exhaustion.

  Preserve: Real margin content, entity visibility, priority, camera wrap, HUD placement and arbitrary Fit resize behavior; no black-margin speedup.

  Evidence: [runner/cyc/cyc_render.c:163](../../runner/cyc/cyc_render.c#L163), [runner/cyc/cyc_render.c:122](../../runner/cyc/cyc_render.c#L122).


- [x] **NES17 · P2 · Retained voxel terrain and sprite assembly**

  Terrain generation, tile materials, sprite grouping and contact shadows are a larger complete job than an isolated triangle helper. Retain static geometry/materials and update changed actors/camera-dependent products.

  Scope: Zelda/Metroid/other supported voxel diorama and first-person modes.

  Evaluation: Zelda exact resolved terrain commands and color/depth retention
  selected at engine `4e8a5e0` / title `fb448b6` (pinned API). Diorama44.31% and
  first-person46.08% CPU reductions each have five faster matched pairs. See
  [retained terrain results](2026-10-28-retained-voxel-results.md). Dynamic actor
  assembly and other title opt-ins remain deferred/unmeasured, explicitly
  carried forward; shared Metroid restore defects are not a disqualifier.
  Complete final combined builds and owner handoff before any merge.

  Preserve: Tile heights, clipping, transparent cards, actor grouping, proportional shadows, HUD and camera/input behavior. Shared Metroid restore defect remains separate.

  Evidence: [runner/src/voxel_renderer.c:575](../../runner/src/voxel_renderer.c#L575), [runner/src/voxel_renderer.c:937](../../runner/src/voxel_renderer.c#L937).


### Seamless world service ownership


- [x] **NES18 · P1 · Persistent neighboring-room simulation service**

  Each eligible neighboring room installs local RAM into an isolated machine, calls translated entity logic, captures it and restores the host. Visible-plus-adjacent activation already exists; making it newly distance-limited is not a new optimization. Build persistent room contexts and a consolidated update job, optionally replacing qualified entity services natively.

  Scope: Zelda Seamless Overworld ordinary Fit, dungeon and atlas modes.

  Decision: Selected isolated native memory/budget service after Fit CPU and dungeon/adaptive/full-map correctness checks. Persistent actors already existed; further scratch contexts are deferred on attribution. See [results](2026-10-28-isolated-native-results.md).

  Preserve: Visible/adjacent actors still update before entry, RNG and timers remain consistent, room handoffs/doors/portals and shared progress remain correct. Slowing simulation is not equivalent performance.

  Evidence: [src/zelda_world_rooms.c:161](https://github.com/mstan/LegendOfZeldaNESRecomp/blob/5d90eb7a10e13f5dd5c693a0204b29f7917ec198/src/zelda_world_rooms.c#L161), [src/zelda_world_rooms.c:251](https://github.com/mstan/LegendOfZeldaNESRecomp/blob/5d90eb7a10e13f5dd5c693a0204b29f7917ec198/src/zelda_world_rooms.c#L251). Reuse `beads-2dw.18.4`.


- [x] **NES19 · P2 · Persistent world raster/material products**

  Room rasters are already cached. Remaining candidates are sharing unchanged world/material products across camera moves, preparing visible runs once and updating actor overlays without repainting every large atlas pixel.

  Scope: Zelda Seamless Fit/dungeon/atlas; bomb/menu/death presentation.

  Evaluation: Complete terrain viewport and old actor rectangle restoration
  selected at Zelda `b2bd8fb`; engine runtime unchanged. Full seamless atlas
  CPU median improves21.83% in five faster pairs; Fit is neutral/inconclusive.
  Eight original/candidate boundary bundles pass, including bomb/menu/death,
  real portals, resizing and current-build loads. See
  [world viewport results](2026-10-30-world-canvas-results.md).
  Camera changes still rebuild; incremental camera-shift reuse is explicitly
  deferred, and upload remains NES24. Complete final combined owner builds.

  Preserve: Bomb-wide flash, inventory/death/Continue framing, arbitrary aspect, palettes, changing tiles and dungeon doors remain correct. Do not reintroduce 4:3 transitions.

  Evidence: [src/zelda_world.c:219](https://github.com/mstan/LegendOfZeldaNESRecomp/blob/5d90eb7a10e13f5dd5c693a0204b29f7917ec198/src/zelda_world.c#L219), [src/zelda_world.c:254](https://github.com/mstan/LegendOfZeldaNESRecomp/blob/5d90eb7a10e13f5dd5c693a0204b29f7917ec198/src/zelda_world.c#L254), [src/zelda_world.c:424](https://github.com/mstan/LegendOfZeldaNESRecomp/blob/5d90eb7a10e13f5dd5c693a0204b29f7917ec198/src/zelda_world.c#L424).


- [x] **NES20 · P1 · Replace full-machine isolation copy/restore with explicit ownership**

  Isolated helpers copy CPU/hardware/cart/PPU/APU/media state and restore it. A declared mutation domain, persistent scratch context or write journal can eliminate repeated full snapshots as part of the room/service architecture.

  Scope: Zelda room updates and other supported isolated guest-service callers.

  Decision: Deferred full snapshot replacement after measuring capture/restore at 3.018% of scoped diagnostic wall time, versus 96.982% guest execution. The larger execution service was selected; this is not architectural exhaustion. See [results](2026-10-28-isolated-native-results.md).

  Preserve: No leaked device clocks, native saves, inventory/shared progress or media/audio changes; refusal of forbidden effects and exact authoritative host continuation.

  Evidence: [runner/cyc/cyc_mod.c:269](../../runner/cyc/cyc_mod.c#L269), [runner/cyc/cyc_state.c:496](../../runner/cyc/cyc_state.c#L496), [runner/cyc/cyc_state.c:530](../../runner/cyc/cyc_state.c#L530).


### State, replay and observation ownership


- [x] **NES21 · P2 · One authoritative state image for save, rewind and mod records**

  State serialization still copies machine/cart/frame products plus host/mod payloads. Prior fused digest and inactive-HD work are already baseline. Evaluate immutable frame images, mutation generations and derived-product reconstruction across the complete capture/restore lifetime.

  Scope: Current-build save/rewind-heavy games; active HD/world records when enabled.

  Evaluation: Stable rewind-preview ownership selected at engine`8df7b50`.
  Four stock title medians improve2.09–4.59% with per-pair variation retained;
  Zelda's opposing exact trials are neutral/inconclusive. Real full-history
  rewind/native-replay/current-load and independent lifetime fixtures pass.
  Existing canonical state/ring ownership is baseline, not newly credited.
  See [rewind history results](2026-10-31-rewind-history-results.md).
  Serializer/directwriter and thumbnail residuals remain deferred, not exhausted.

  Preserve: Current-build save/load/rewind correctness and malformed-input rejection; persistent native saves protected. Old-state compatibility, old layout and private cache byte identity are not requirements.

  Evidence: [runner/cyc/cyc_state.c:104](../../runner/cyc/cyc_state.c#L104), [runner/cyc/cyc_state.c:82](../../runner/cyc/cyc_state.c#L82).


- [ ] **NES22 · P2 · Rollback capture/digest/replay ownership**

  The prior fused digest and presentation-skipping replay must not be credited again. Remaining candidates are common authoritative capture ownership, changed-region hashing and avoiding duplicate host/mod reconstruction during actual delayed-peer replay.

  Scope: Actual delayed-peer cycle netplay and current-build replay.

  Next: Only if netplay is selected: one real delayed-peer workload with actual replay ticks and full endpoint-state/digest checks. Do not infer rollback cost from offline snapshots.

  Preserve: Deterministic peer input, current-build restore, all authoritative state and before-frame-0 capability/profile agreement; same-host checks do not establish cross-build compatibility.

  Evidence: [runner/cyc/cyc_net.c:84](../../runner/cyc/cyc_net.c#L84), [runner/cyc/cyc_session.c:197](../../runner/cyc/cyc_session.c#L197). Reuse `beads-2dw.1.100`.


- [x] **NES23 · P2 · Observer/history work integrated with semantic events**

  Offline timed builds disable optional cycle tracing, but ordinary device history remains active. Netplay explicitly enables cycle tracing as a simulation setting. Derive required history/digests at semantic service boundaries and separate them from trace-detail production; preserve useful diagnostics rather than pretending legacy CMake keys disable the cycle ring.

  Scope: Offline production history and netplay trace/digest consumers, measured separately.

  Evaluation: Offline rewrite deferred after actual stockSMB3/fullZelda-atlas
  attribution. Existing semantic aggregation remains baseline; instrumented
  outermost ring bodies account for about0.035% observed wall, not a recoverable
  bound or an A/B gain. Both RAM-interpreter counts are0. See
  [observer results](2026-10-31-observer-results.md). Broader bookkeeping and
  separately qualified common NET profiles remain explicit residuals.

  Preserve: Required fault information, ring consumers, hooks and agreed peer digests remain correct. Do not disable netplay hashing unilaterally; establish a common validated profile before changing its simulation identity.

  Evidence: [runner/cyc/cyc_ring.c:24](../../runner/cyc/cyc_ring.c#L24), [runner/cyc/cyc_net.c:56](../../runner/cyc/cyc_net.c#L56).


### Host presentation and compiler policies


- [x] **NES24 · P2 · Frame conversion, texture upload and GPU residency**

  Compositor generation reuse exists, but every shown picture is still fully
  uploaded. Evaluate conversion and upload together with the selected producer.

  Scope: Actual SDL stock/wide/HD/voxel presentation; software and selected GPU backend.

  Evaluation: Two pending-damage/retained-texture prototypes deferred after
  actual full seamless-atlas software/D3D11 screening. v1 always hit rectangle
  fallback; v2 reduced bytes but increased CPU/upload cost. Independent pixel
  oracles pass; no runtime adoption. See [upload results](2026-10-31-present-upload-results.md).
  Indexed conversion and alternate resident producer/API designs remain explicit
  unmeasured residuals; no third tiled-upload tuning trial.

  Preserve: Palette/emphasis, screenshots, filters, resize, UI/HUD and output latency; retain software presentation and supported-platform fallback.

  Evidence: [runner/cyc/cyc_render.c:163](../../runner/cyc/cyc_render.c#L163), [runner/cyc/cyc_sdl.c:1203](../../runner/cyc/cyc_sdl.c#L1203).


- [ ] **NES25 · P3 · Audio host buffering and queue ownership**

  Separate generated PCM cost from draining/copying/queueing output. Consolidate buffers or callback ownership only when they cause meaningful CPU or underruns; the earlier desktop queue share was small.

  Scope: Real audio-device gameplay; queues/underruns distinct from dummy output timing.

  Next: Observe queue depth, underruns and output-thread CPU on actual audio-on gameplay, separately from dummy-device simulation timing.

  Preserve: PCM, latency, pause/resume, volume and fast-forward behavior; muting or disabling emulation is not an optimization win.

  Evidence: [runner/cyc/cyc_sdl.c:269](../../runner/cyc/cyc_sdl.c#L269), [runner/cyc/hw_apu.c:192](../../runner/cyc/hw_apu.c#L192).


- [ ] **NES26 · P3 · Pacing and presentation deadlines**

  A previous waitable-pacing candidate reduced CPU but worsened deadline lateness. A revised service can coordinate audio/video/input deadlines without busy wait or repeated synchronous presentation.

  Scope: Normal paced play, fast-forward and slow-display presentation.

  Next: Reuse the negative receipt first; only test a materially different deadline design. Compare p95/p99 lateness, cadence and input latency as well as CPU.

  Preserve: No worse user-visible pacing/latency or broken fast-forward. Retain the earlier failed candidate as rejected, not an open ready-to-ship win.

  Evidence: [runner/cyc/cyc_sdl.c:118](../../runner/cyc/cyc_sdl.c#L118), [runner/cyc/cyc_frame_pacing.h:9](../../runner/cyc/cyc_frame_pacing.h#L9).


- [ ] **NES27 · P2 · Compiler policy and generated-code layout** (`open-existing-IPO-defect`)

  PGO, hot/cold layout, translation-unit/block layout and supported selective IPO can improve native locality and cross-unit optimization. Global IPO has a real SMB1 Fit divergence; native-C-only IPO had functional support but no admitted timing gain.

  Scope: SMB1 Fit defect; current gameplay-trained compiler policies across affected consumers.

  Next: Reuse beads-2dw.1.106 and existing binaries. Diagnose the actual aliasing/lifetime/optimizer defect before global IPO; use current gameplay-trained PGO or layout candidates only after the larger pipeline decision.

  Preserve: Every affected game remains neutral or positive and correct; generated coverage/build size are not vetoes by themselves, but startup/memory costs must be recorded.

  Evidence: [recompiler/src/cyc_codegen.c:76](../../recompiler/src/cyc_codegen.c#L76), [runner/cyc/project.cmake:150](../../runner/cyc/project.cmake#L150). Reuse `beads-2dw.1.106`.


- [ ] **NES28 · P3 · Parallel immutable visual jobs**

  After ownership consolidation, independent tile preparation, HD composition or voxel work can use immutable frame products on workers. Adding threads to shared mutable machine state is not the first step.

  Scope: A measured independent HD/wide/voxel visual job after immutable ownership exists.

  Next: Only after measuring a large parallelizable render job: compare complete frame latency, CPU and RSS against the simpler serial replacement.

  Preserve: Deterministic authoritative simulation, no races/lifetime errors, bounded queues and acceptable latency; serial portable fallback remains usable.

  Evidence: [runner/cyc/cyc_render.c:12](../../runner/cyc/cyc_render.c#L12), [runner/src/voxel_renderer.c:1147](../../runner/src/voxel_renderer.c#L1147).


### Conditional device and startup work


- [ ] **NES29 · P3 · FDS BIOS, disk transfer and RAM publication as native services** (`parked-owner-excluded-FDS`)

  Cartridge NES games have no common BIOS to consolidate. FDS does: current HLE observes ID/load spans, auto-swaps and removes host pacing during loading while retaining the low-level drive/recompiled BIOS. A true BIOS/media service and loaded-image preparation are a separate larger opportunity.

  Scope: FDS inventory only: Otocky/Esper Dream/Nazo no Murasame-jou when owner reopens scope.

  Next: Inventory only. Earlier owner instructions excluded further FDS validation; no FDS launch or promotion unless that scope is reopened. Then select one actual loading-to-gameplay interval.

  Preserve: Disk files/status/errors, writes/native progress, side routing, RAM publication and game-visible completion/interrupt timing; distinguish CPU savings from an elective faster-loading behavior change.

  Evidence: [runner/cyc/hw_fds_hle.c:2](../../runner/cyc/hw_fds_hle.c#L2), [runner/cyc/cyc_core.h:104](../../runner/cyc/cyc_core.h#L104). Reuse `beads-2dw.1.74`.


- [ ] **NES30 · P3 · Expansion-audio and complex-board event services**

  FDS/VRC6/VRC7/Namco/FME-7 and MMC5-specific work has different timer, sampling and bus-observer costs. Build event-driven board/audio services where an actually supported title spends time, not a generic callback cleanup.

  Scope: A supported title using MMC5 or VRC/Namco/FME-7 expansion services; FDS parked.

  Next: A matching supported board workload and one inclusive service attribution. Reuse existing board fixtures; no all-mapper matrix merely to rank a candidate.

  Preserve: Expansion PCM, board IRQ/read gating and register timing, device state and portable execution. FDS-specific work remains parked with NES29.

  Evidence: [runner/cyc/hw_mapper.c:1301](../../runner/cyc/hw_mapper.c#L1301), [runner/cyc/hw_mapper.c:1367](../../runner/cyc/hw_mapper.c#L1367).


- [ ] **NES31 · P3 · Startup, native code footprint and first-use stalls**

  Larger generated programs and boot-time RAM-view indexes can affect startup, RSS and first-use page faults even when steady gameplay is fast. Deduplicate cold metadata and lay out hot native blocks as one startup/execution design.

  Scope: A supported title with reported/measured startup or first-use stalls.

  Next: Only if startup or first-use delay is reported or visible in the selected interval; distinguish generation/build time, launcher time and game runtime. No menu-only throughput campaign.

  Preserve: Same ROM/content validation, working fallback, actual game start and persistent settings/saves. Longer generation and a larger binary alone do not disqualify useful built-runtime gains.

  Evidence: [recompiler/src/cyc_codegen.c:15](../../recompiler/src/cyc_codegen.c#L15), [runner/cyc/cyc_ramview.c:134](../../runner/cyc/cyc_ramview.c#L134).


## Evidence reuse and stopping rules

- Keep the October 27 accepted gains as delivered results, not the residual
  cost profile. Current engine and Zelda features have advanced since them.
- Reuse the October 22 rejected packed-priority, allocation/lazy-scratch and
  waitable-pacing receipts; do not repeat unchanged experiments. Global IPO's
  SMB1 Fit divergence is introduced and remains a valid correctness blocker.
- The old RAM-view feature needs a correct mapping/write contract; simply
  enabling the FDS flag is unsafe. That is an implementation problem to solve,
  not a permanent ban on native cartridge RAM.
- Perform the smallest attribution that selects one complete job. Implement
  and integrate it promptly, then compare ordinary built gameplay. Profiling,
  counters and coverage are not a final performance result. No automatic
  all-title/all-mode benchmark matrix or artificial minimum percentage.
- Qualify directly affected contracts and representative consumers using
  existing deterministic routes; compare A/B without observers in timed runs.
  The owner explicitly authorized measurements on shared hosts, including the
  workstation. Record contention and retain raw samples; do not reinstate a
  quiet-host admission gate or alter foreign jobs/leases. After two failed improvement
  experiments, reassess the architecture before more tuning.
- A game can be neutral while another improves. Reject/fix a reproducible
  per-game slowdown; mixed changes inside variation remain inconclusive.
  Identical original/candidate failures, including the tracked Metroid voxel
  restore presentation defect, are separate. Do not broaden unrelated fixes
  into the optimization acceptance gate.
- Preserve host/ROM/source/binary/settings/package identities, useful workload
  interval and raw CPU/frame/stall/memory samples. Report CPU-time reduction
  separately from throughput. FDS remains inventory-only under the earlier
  owner exclusion. No new owner playtest or release is needed for this audit.

Legacy-only work is conditional on a real supported consumer using that host;
do not spend current cycle-game time optimizing an unused older runner.
CPU/OS/ABI-specific assembly or SIMD is allowed with feature checks and a
working portable implementation. Netplay settles a common validated profile
before frame 0; platform compilation alone is not compatibility qualification.

Audit completion closes only `beads-2dw.1.109`'s investigation/deliverable.
The thirty-one opportunities and existing child issues remain unresolved;
this is not an architectural-exhaustion or new performance-gain claim.
