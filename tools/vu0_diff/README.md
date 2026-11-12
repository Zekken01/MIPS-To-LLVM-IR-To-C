# PCSX2 VU0 interpreter differential tests

This directory contains a test adapter for the pinned PCSX2 interpreter and
the shared deterministic microprogram used to compare it with the in-tree VU0
runtime.

## Pinned reference

The adapter is pinned to PCSX2 commit
82cac38b2af19d593dedfd9ddaa63448e7ec493c. The relevant interpreter
implementation is
[pcsx2/VU0microInterp.cpp](https://github.com/PCSX2/pcsx2/blob/82cac38b2af19d593dedfd9ddaa63448e7ec493c/pcsx2/VU0microInterp.cpp).

The adapter retains three focused issue-order probes:

1. A one-cycle VI backup expires during a three-cycle lower-IALU interlock
   before IBEQ evaluates its operands.
2. IBEQ with two pending source reservations waits for the later completion
   (three versus five cycles).
3. A scalar IALU write starts a fresh two-cycle VI-backup window after the
   interpreter's pre-handler aging phase.

It also has an opt-in per-pair trace test. That test executes a shared 56-pair
program through vu0Exec(&VU0) one pair at a time and writes architectural
state after each pair. The fixture executes all seven supported scalar lower
operations, B/BAL/JR/JALR, all six conditional branches, an M-marked pair, and
an E-marked pair plus its delay pair.

## Install into a PCSX2 checkout

Use a clean checkout at the pinned revision. The installer refuses another
revision and changes only the PCSX2 core test CMake source list plus the adapter, shared
fixture, and SHA-256 helper source files.

~~~sh
git clone https://github.com/PCSX2/pcsx2.git pcsx2-vu0-reference
git -C pcsx2-vu0-reference checkout 82cac38b2af19d593dedfd9ddaa63448e7ec493c
python3 tools/vu0_diff/integrate_pcsx2_test.py --pcsx2-root pcsx2-vu0-reference
~~~

Configure and build the existing PCSX2 core test target using the toolchain
and system dependencies from PCSX2's normal Linux build instructions. On
Linux, keep X11 enabled even with OpenGL/Vulkan and Wayland disabled: the
pinned PCSX2 common target links X11::X11, X11::Xrandr and X11::Xi.

~~~sh
cmake -S pcsx2-vu0-reference -B pcsx2-vu0-reference/build-vu0-diff -G Ninja \
    -DENABLE_TESTS=ON -DENABLE_QT_UI=OFF -DX11_API=ON -DWAYLAND_API=OFF \
    -DCMAKE_BUILD_TYPE=Release
cmake --build pcsx2-vu0-reference/build-vu0-diff --target core_test --parallel 2
~~~

## Produce and compare traces

The one-command driver installs the PCSX2 adapter, configures and builds both
projects, emits both traces, and compares them:

~~~sh
python3 tools/vu0_diff/run_differential.py --pcsx2-root /path/to/pcsx2-vu0-reference
~~~

There is also a manual GitHub Actions workflow named **VU0 PCSX2 differential**.
Open the repository's Actions tab, select that workflow, and choose **Run
workflow**. It checks out the pinned PCSX2 source, builds its dependencies and
core test, compares the traces, and uploads both JSONL files even on failure.


Use --keep-traces to retain both JSONL files after a successful comparison.
The driver does not install system packages; first satisfy PCSX2's normal
build dependencies. The project and PCSX2 build directories can be overridden
with --project-build-dir and --pcsx2-build-dir. Pass --pcsx2-prefix-path
when the PCSX2 dependencies were installed outside its source tree; otherwise
the driver automatically uses <pcsx2-root>/deps when that directory exists.

For manual operation, build this repository first. The candidate trace
producer is built as ee_vu0_trace_candidate.

On Linux/macOS:

~~~sh
build/ee_vu0_trace_candidate build/candidate-vu0.jsonl

VU0_TRACE_OUTPUT="$PWD/build/pcsx2-vu0.jsonl" \
    pcsx2-vu0-reference/build-vu0-diff/bin/core_test \
    --gtest_filter='VU0IssueOrder.EmitDeterministicPerPairTrace'

python3 tools/compare_vu0_traces.py \
    build/pcsx2-vu0.jsonl build/candidate-vu0.jsonl
~~~

On PowerShell:

~~~powershell
.\build\ee_vu0_trace_candidate.exe .\build\candidate-vu0.jsonl
$env:VU0_TRACE_OUTPUT = (Resolve-Path .\build).Path + "\pcsx2-vu0.jsonl"
.\pcsx2-vu0-reference\build-vu0-diff\bin\core_test.exe --gtest_filter="VU0IssueOrder.EmitDeterministicPerPairTrace"
python tools\compare_vu0_traces.py .\build\pcsx2-vu0.jsonl .\build\candidate-vu0.jsonl
~~~

The PCSX2 test skips trace generation when VU0_TRACE_OUTPUT is not set, so
the normal PCSX2 core test suite does not write files. The focused issue-order
tests still run normally.

The comparator checks program identity, initial state, executed pair PC,
lower/upper instruction words, all 32 VI values, cycle count, branch/delay
state, VI backup state, E/M state and running status. It reports every
architectural difference and returns nonzero when traces differ.

## Limits

This fixture executes 49 pairs from a 56-pair program and covers the lower
scalar integer/branch subset plus ordinary VU0 ILW/ISW and masked LQ/SQ memory operations,
with upper NOP. The comparator checks all 4 KB of VU0 data memory using a
SHA-256 digest and all 32 VF register lane patterns after each pair. The pinned 45-pair comparison passed in
differential run #9 (47 cycles). The new 49-pair fixture has not yet been
confirmed against the pinned interpreter. Memory-mapped VU1 register accesses, other
VU memory operations, upper vector instructions, dual-issue dependencies,
VF forwarding and FMAC/FDIV/EFU pipelines remain out of scope.
