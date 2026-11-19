# Parallel CTest CI shards

The main Build and test workflow builds **once**, retaining the existing
release configuration, LLVM Python binding and "ee-recomp-linux" tool artifact.

The build job uploads the complete configured build and Python virtual
environment in a tarball. Tar preserves executable bits and symlinks.

Four independent test jobs with `needs: build` use the same archive:

- `game`: the long-running `game_runner_end_to_end` test.
- `fast-0`, `fast-1`, `fast-2`: all other configured CTest tests.

Each job checks out source, restores the archive at the same workspace path,
and invokes the existing CTest configuration without rebuilding. The Python
selector uses `ctest --show-only=json-v1`, checks exhaustive test coverage,
and selects exact names with an anchored regex. New tests automatically join
a fast shard; no fixed test-number ranges are required.

The matrix has `fail-fast: false`, and every shard uploads its JUnit output
and LastTest.log. The workflow passes only after all four test jobs pass.
Other ROM0, SIF DMA and WebGPU workflows remain unchanged.

Run a shard locally after building:

```bash
python3 tools/ci/ctest_shard.py --build-dir build --shard fast-0
python3 tools/ci/ctest_shard.py --build-dir build --shard game
```
