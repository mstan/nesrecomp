Timestamp: 2026-04-08 16:10 America/Los_Angeles
Author: Codex

# Function Finder Heuristic Summary

This summary was written by Codex during the Zelda/Metroid structural-analysis pass on branch `feature/function-finder`.

## Goal

The goal of this work was to move the function finder away from raw recall and toward structural correctness:

- avoid fake standalone functions
- avoid splitting in-body secondary entries into standalone roots
- preserve user-authored `game.toml`
- improve first-pass config generation so users mostly add missing functions instead of deleting bad guesses

## Main Code Changes

Files changed:

- `recompiler/src/function_finder.h`
- `recompiler/src/function_finder.c`
- `recompiler/src/code_generator.c`
- `recompiler/src/main_nes.c`

Key changes:

- Added provenance tracking to discovered function entries:
  - `kind`
  - `canonical_addr`
  - `canonical_bank`
  - `source_flags`
  - `evidence_count`
- Split discovery sources more explicitly:
  - `control`
  - `ptr_scan`
  - `table_run`
  - `split_table`
  - `known_table`
  - `xbank`
  - `manual`
  - `bank_seed`
- Stopped laundering weak discovery into strong control-flow:
  - bank seeds are tracked separately
  - uncertain-bank fanout uses `xbank`
  - propagated discoveries inherit weaker provenance when appropriate
- Added structural secondary classification and canonical ownership logic
- Added audit CSV output for all auto-discovered entries
- Added proposal generation support through `--proposal-out`
- Changed proposal emission to be more selective by kind/source

## Config Generation Direction

The current direction is:

- standalone `[functions]` should remain conservative
- `[[extra_label]]` can be generated more aggressively when evidence suggests a valid in-body entry
- uncertain discoveries should not be promoted to standalone roots by default

This still needs more work, but it is closer to the intended “best effort first draft” model.

## Zelda Validator Results

Zelda was used as the precision oracle because its authoritative `game.toml` is structurally trustworthy.

Validator artifacts:

- `F:\\Projects\\nesrecomp-release\\LegendOfZeldaNESRecomp\\game.discovery.toml`
- `F:\\Projects\\nesrecomp-release\\LegendOfZeldaNESRecomp\\game.discovery.generated.toml`
- `F:\\Projects\\nesrecomp-release\\LegendOfZeldaNESRecomp\\game.discovery.compare.md`

High-level comparison:

| Measure | Old Aggressive | Prior Conservative | Current Middle Ground | Zelda Oracle |
|---|---:|---:|---:|---:|
| Standalone emitted | 4533 | 70 | 70 | 137 |
| `extra_label` emitted | 0 | 0 | 127 | 101 |
| Correct function matches | 51 | 25 | 25 | 137 |
| Correct label matches | 0 | 0 | 92 | 101 |
| Total semantic matches | 51 | 25 | 117 | 238 |
| Dangerous extra standalones | 4482 | 45 | 45 | 0 |
| Extra labels not in oracle | 0 | 0 | 35 | 0 |

Interpretation:

- The old approach was unusably aggressive.
- The first precision pass was much safer, but too conservative.
- The current middle-ground pass keeps standalone roots conservative while recovering most Zelda labels.

## Important Current State

What is better now:

- far fewer dangerous standalone false positives than the old heuristic
- much better label recovery on Zelda
- proposal generation can now be tested against helper configs without overwriting authoritative configs

What is still missing:

- many real Zelda standalone functions are still not recovered
- remaining false standalone roots still need to be pruned further
- secondary classification is still not generally strong enough outside the SRAM-translated Zelda case

## Recommended Next Step

The next pass should target:

- the remaining `45` false Zelda standalone roots
- recovery of missing true Zelda standalone functions
- keeping the current `92/101` Zelda label recovery intact

The important constraint is unchanged:

- tolerate dead-code noise more than structural corruption
- do not re-open the old “thousands of fake standalone roots” behavior
