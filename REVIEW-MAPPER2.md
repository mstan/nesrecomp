# Draft: Guardian Legend Mapper 2 legacy-runtime support

Base: upstream NESRecomp `a39d9cd`; independent of the macOS proposal.

## Review order
1. Recompiler/UxROM continuation and interpreter semantics, with codegen tests.
2. PPU vertical reload and vblank synchronization behavior.
3. Scripted save-state hotkeys and opt-in diagnostics used to investigate
   death/retry and reload failures.

These changes originated in The Guardian Legend port pinned to `1c7f3a2`.
The save-state ghost-tail reset is already in current upstream and is not
resubmitted. The test helper explicitly selects `--legacy` because these tests
inspect function/frame output, whereas upstream now defaults to cycles.

## Validation and remaining work
- Recompiler builds with AppleClang; all 78 Vitest tests pass on this branch.
- Original pinned build: first-level shooting/combat regression and manual
  repeated slot-9 load/death/retry play were previously successful.
- Full runtime/gameplay validation against this newer upstream base remains
  outstanding. Historical gameplay results are not proof for this rebase.
- In particular, review the post-NMI vblank re-arm against other games and
  both renderer modes before merging; its behavior is broader than Mapper 2.
- The diagnostic ring is opt-in but adds a disabled-path check; no claim of
  zero overhead is made. Temporary tracing can be omitted by dropping its
  separate commit.

No watchdog limits or stack guards were weakened. Game-specific native roots
remain in the Guardian Legend repository rather than this engine proposal.
