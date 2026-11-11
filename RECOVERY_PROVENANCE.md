# Recovery Provenance — 2026-11-11

The previous ChatGPT conversation's temporary working directory was not exported before that conversation became inaccessible. This branch therefore starts from the latest source artifact preserved in the Library, not from the inaccessible live worktree.

## Source inputs

- `PS2_EE_Recompiler_Reviewed_Bundle_2026-11-11.zip`: older reviewed source baseline.
- `PS2_EE_Recompiler_MMI_HI1_LO1_Followup_2026-11-11.zip`: newer 09:57 UTC source follow-up, including MMI/HI1/LO1 source changes, regression tests, notes, and patch.
- `PS2_EE_Recompiler_Worktree_2026-11-11_1430.zip`: inspected but found to contain the older 05:37 reviewed source and three-test CTest setup, despite its filename/time. It is not the source of this recovery branch.

The 09:57 follow-up was applied over the reviewed baseline. Its source changes include the MMI lifting/lowering, secondary HI1/LO1 transfer handling, CFG liveness/writeback adjustments, and removal of unsafe custom level-1 store elimination.

## Important limitation

The exact later worktree associated with the reported 2/4 CTest run was never exported. Its uncommitted changes and logs are not present in the preserved files. This branch must be treated as a reconstructed baseline, not as a recovery of that exact live worktree.

The reconstructed source was clean-built and tested in the current environment. See `TEST_RESULTS.md`.
