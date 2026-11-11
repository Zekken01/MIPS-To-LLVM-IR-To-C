# Work Log

## 2026-11-11 — Recovery baseline

- Confirmed private GitHub repository access and created this dedicated recovery branch from the initial repository README.
- Recovered the latest source follow-up preserved in the Library (09:57 UTC).
- Inspected the separately named 14:30 worktree archive; despite its name it contains the older reviewed source and does not contain the newer four-test state.
- Reconstructed the source from the 09:57 follow-up over the reviewed baseline.
- Performed a clean CMake/Ninja build and ran CTest: 4/4 passing; decoder regressions: 269/269.
- The exact later 2/4 worktree and logs remain unrecovered because the previous chat never exported them.

Further changes should be committed to this branch in small, testable increments. Do not merge it into `main` until the source and test baseline have been reviewed.
