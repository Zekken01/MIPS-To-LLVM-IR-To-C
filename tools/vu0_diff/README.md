# PCSX2 VU0 issue-order oracle

This directory contains the first executable PCSX2-side oracle for the lower
integer/branch slice. It is deliberately separate from the recompiler runtime:
the adapter is added to a PCSX2 test build and calls PCSX2's existing
interpreter, while the recompiler continues to have no PCSX2 runtime dependency.

## Pinned reference

The adapter is pinned to PCSX2 commit
`82cac38b2af19d593dedfd9ddaa63448e7ec493c`. The source ordering being
validated is in
[pcsx2/VU0microInterp.cpp](https://github.com/PCSX2/pcsx2/blob/82cac38b2af19d593dedfd9ddaa63448e7ec493c/pcsx2/VU0microInterp.cpp#L924-L1020).

The adapter checks three boundary cases directly against `vu0Exec(&VU0)`:

1. A one-cycle VI backup is expired by a three-cycle lower-IALU interlock
   before IBEQ evaluates its operands.
2. IBEQ with two pending source reservations waits for the later completion
   (three versus five cycles), then evaluates the current VI values.
3. A scalar IALU write starts a fresh two-cycle VI-backup window after the
   interpreter's pre-handler aging phase.

These probes validate the upstream interpreter behavior that the recompiler
runtime now mirrors. They are not yet a full per-pair trace comparison; the
next stage is to give both executables a shared fixture format and compare
architectural-state traces automatically.

## Install into a PCSX2 checkout

Use a clean checkout at the pinned revision. The installer refuses any other
revision and changes only PCSX2's `tests/ctest/core/CMakeLists.txt` plus the
new adapter source file.

```sh
git clone https://github.com/PCSX2/pcsx2.git pcsx2-vu0-reference
git -C pcsx2-vu0-reference checkout 82cac38b2af19d593dedfd9ddaa63448e7ec493c
python3 tools/vu0_diff/integrate_pcsx2_test.py --pcsx2-root pcsx2-vu0-reference
```

Configure and build the existing PCSX2 core test target using the toolchain and
system dependencies from PCSX2's normal Linux build instructions:

```sh
cmake -S pcsx2-vu0-reference -B pcsx2-vu0-reference/build-vu0-diff -G Ninja \
    -DENABLE_TESTS=ON -DENABLE_QT_UI=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build pcsx2-vu0-reference/build-vu0-diff --target core_test --parallel 2
pcsx2-vu0-reference/build-vu0-diff/bin/core_test --gtest_filter='VU0IssueOrder.*'
```

On Windows, run the corresponding `core_test.exe` from the configured build
directory. If your PCSX2 configuration uses a different runtime output path,
locate the generated `core_test` executable and pass the same GoogleTest
filter.

The source adapter uses PCSX2's existing CTest target and global VU0 state. It
does not copy or modify the PCSX2 interpreter implementation. Remove the
adapter from the checkout with:

```sh
python3 tools/vu0_diff/integrate_pcsx2_test.py --pcsx2-root pcsx2-vu0-reference --remove
```
