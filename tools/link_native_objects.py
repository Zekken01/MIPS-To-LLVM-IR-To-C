#!/usr/bin/env python3
"""Link emitted per-function host objects with the EE runtime and PC dispatcher.

The input metadata is ee_functions.json from ee_recomp. The generated shared
library exports ee_game_run(cpu, runtime, budget, out_count, out_pc, out_status).
It embeds the guest block-PC ownership map and a parallel array of native
function pointers, then delegates to ee_dispatch_run().

This produces a linked translated-code library. It does not provide BIOS, IOP,
GS, DMA, TLB/MMIO devices, or a complete console host.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path


def cpp_string(value):
    return (
        '"'
        + str(value).replace("\\", "\\\\").replace('"', '\\"')
        .replace("\n", "\\n").replace("\r", "\\r")
        + '"'
    )


def parse_u32(value, field):
    try:
        number = int(value, 0) if isinstance(value, str) else int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"invalid {field}: {value!r}") from exc
    if number < 0 or number > 0xFFFFFFFF:
        raise ValueError(f"{field} is outside uint32: {value!r}")
    return number


def build_dispatch_source(metadata, functions):
    lines = [
        '#include "ee_dispatcher.h"',
        "#include <cstdint>",
        "#include <utility>",
        "",
        "extern \"C\"",
        "{",
    ]
    for fn in functions:
        lines.append(
            f"    std::uint32_t {fn['host_symbol']}("
            "ee::s_ee_cpu_state*, ee::s_ee_runtime*);"
        )
    lines += [
        "}",
        "",
        "namespace",
        "{",
        "ee::s_recompiled_elf make_compiled_metadata()",
        "{",
        "    ee::s_recompiled_elf compiled{};",
        f"    compiled.entry_point = 0x{parse_u32(metadata.get('entry_point'), 'entry_point'):08x}u;",
        f"    compiled.function_count = {len(functions)}u;",
        f"    compiled.functions_discovered = {parse_u32(metadata.get('functions_discovered', len(functions)), 'functions_discovered')}u;",
    ]

    for fn in functions:
        block_values = ", ".join(f"0x{pc:08x}u" for pc in fn["block_pcs"])
        lines += [
            "    {",
            "        ee::s_recompiled_fn function{};",
            f"        function.name = {cpp_string(fn.get('name', ''))};",
            f"        function.host_symbol = {cpp_string(fn['host_symbol'])};",
            f"        function.vaddr = 0x{fn['vaddr']:08x}u;",
            f"        function.block_count = {len(fn['block_pcs'])}u;",
            f"        function.block_pcs = {{{block_values}}};",
            f"        function.ir_path = {cpp_string(fn.get('ir_path', ''))};",
            "        compiled.functions.push_back(std::move(function));",
            "    }",
        ]

    lines += [
        "    return compiled;",
        "}",
        "",
        "const ee::s_recompiled_elf g_compiled = make_compiled_metadata();",
        "const ee::t_ee_translated_entry g_entries[] =",
        "{",
    ]
    for fn in functions:
        lines.append(f"    &{fn['host_symbol']},")
    lines += [
        "};",
        "const ee::s_ee_dispatch_table g_dispatch_table",
        "{",
        "    &g_compiled,",
        "    g_entries,",
        "    sizeof(g_entries) / sizeof(g_entries[0])",
        "};",
        "} // namespace",
        "",
        'extern "C" std::uint32_t ee_game_run(',
        "    ee::s_ee_cpu_state* cpu,",
        "    ee::s_ee_runtime* runtime,",
        "    std::uint32_t max_dispatches,",
        "    std::uint32_t* out_dispatch_count,",
        "    std::uint32_t* out_guest_pc,",
        "    std::uint32_t* out_status)",
        "{",
        "    const ee::s_ee_dispatch_result result =",
        "        ee::ee_dispatch_run(cpu, runtime, g_dispatch_table, max_dispatches);",
        "    if (out_dispatch_count) *out_dispatch_count = result.dispatch_count;",
        "    if (out_guest_pc) *out_guest_pc = result.guest_pc;",
        "    if (out_status) *out_status = static_cast<std::uint32_t>(result.status);",
        "    return result.exit_reason;",
        "}",
        "",
    ]
    return "\n".join(lines)


def load_inputs(metadata_path, object_dir):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    object_manifest_path = object_dir / "objects.json"
    if not object_manifest_path.is_file():
        raise ValueError(f"object manifest not found: {object_manifest_path}")

    object_manifest = json.loads(object_manifest_path.read_text(encoding="utf-8"))
    if not object_manifest.get("complete"):
        raise ValueError("objects.json does not report a complete emission")
    if object_manifest.get("target_triple") != object_manifest.get("target_triple", "").strip():
        raise ValueError("object manifest has an invalid target triple")

    functions = metadata.get("functions")
    objects = object_manifest.get("objects")
    if not isinstance(functions, list) or not functions:
        raise ValueError("ee_functions.json contains no functions")
    if not isinstance(objects, list) or len(objects) != len(functions):
        raise ValueError("object count does not match function metadata")

    object_by_input = {}
    for item in objects:
        input_name = Path(item["input"]).name
        if input_name in object_by_input:
            raise ValueError(f"duplicate object manifest input: {input_name}")
        object_by_input[input_name] = item

    linked_objects = []
    validated_functions = []
    seen_symbols = set()

    for function in functions:
        symbol = function.get("host_symbol", "")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol):
            raise ValueError(f"invalid or missing host_symbol for {function.get('name')!r}")
        if symbol in seen_symbols:
            raise ValueError(f"duplicate native function symbol: {symbol}")
        seen_symbols.add(symbol)

        ir_name = Path(function.get("ir_path", "")).name
        item = object_by_input.get(ir_name)
        if item is None:
            raise ValueError(f"no object manifest entry for IR module {ir_name}")
        if symbol not in item.get("defined_symbols", []):
            raise ValueError(f"object {item['object']} does not define {symbol}")

        object_path = object_dir / item["object"]
        if not object_path.is_file():
            raise ValueError(f"object file missing: {object_path}")
        data = object_path.read_bytes()
        if len(data) != item["size_bytes"]:
            raise ValueError(f"object size mismatch: {object_path}")
        digest = hashlib.sha256(data).hexdigest()
        if digest != item["sha256"]:
            raise ValueError(f"object digest mismatch: {object_path}")

        block_pcs = [
            parse_u32(pc, f"{symbol}.block_pcs") for pc in function.get("block_pcs", [])
        ]
        if not block_pcs:
            raise ValueError(f"function {symbol} has no dispatchable block PCs")
        if len(block_pcs) != len(set(block_pcs)):
            raise ValueError(f"function {symbol} has duplicate block PCs")

        validated = {
            "name": function.get("name", ""),
            "host_symbol": symbol,
            "vaddr": parse_u32(function.get("vaddr"), f"{symbol}.vaddr"),
            "block_pcs": block_pcs,
            "ir_path": function.get("ir_path", ""),
        }
        validated_functions.append(validated)
        linked_objects.append(object_path)

    if len(functions) != parse_u32(metadata.get("function_count", len(functions)), "function_count"):
        raise ValueError("function_count does not match the function array")

    return metadata, object_manifest, validated_functions, linked_objects


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("functions_json", type=Path, help="ee_functions.json from ee_recomp")
    parser.add_argument("object_dir", type=Path, help="directory containing .o files and objects.json")
    parser.add_argument("output", type=Path, help="output shared library path")
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    try:
        metadata, object_manifest, functions, objects = load_inputs(
            args.functions_json.resolve(), args.object_dir.resolve()
        )

        required_sources = [
            source_root / "src/runtime/ee_memory.cpp",
            source_root / "src/runtime/ee_exceptions.cpp",
            source_root / "src/ee/recompiled_functions.cpp",
            source_root / "src/ee/ee_dispatcher.cpp",
        ]
        for source in required_sources:
            if not source.is_file():
                raise ValueError(f"required runtime source not found: {source}")

        args.output.parent.mkdir(parents=True, exist_ok=True)
        dispatch_source = args.output.with_suffix(args.output.suffix + ".dispatch.cpp")
        dispatch_source.write_text(
            build_dispatch_source(metadata, functions), encoding="utf-8"
        )

        command = [
            args.cxx,
            "-std=c++17",
            "-O2",
        ]
        if sys.platform == "linux":
            command += ["-shared", "-fPIC", "-Wl,--no-undefined"]
        elif sys.platform == "darwin":
            command += ["-dynamiclib", "-fPIC"]
        elif sys.platform == "win32":
            command += ["-shared"]
        else:
            raise ValueError(f"unsupported host platform for shared-library linking: {sys.platform}")

        command += [
            "-I" + str(source_root / "include"),
            "-I" + str(source_root / "include/ee"),
            "-I" + str(source_root / "src/runtime"),
        ]

        # Use a response file: the complete game emits thousands of per-function
        # objects and can exceed the operating system's command-line length.
        response_path = args.output.with_suffix(args.output.suffix + ".objects.rsp")
        response_path.write_text(
            "\n".join('"' + str(path).replace('"', '\\"') + '"' for path in objects)
            + "\n",
            encoding="utf-8",
        )
        command.append("@" + str(response_path))
        command += [str(path) for path in required_sources]
        command.append(str(dispatch_source))
        command += ["-o", str(args.output)]

        print(
            f"link_native_objects: linking {len(objects)} guest objects "
            f"for {object_manifest['target_triple']} -> {args.output}",
            flush=True,
        )
        subprocess.run(command, check=True)

        linked_data = args.output.read_bytes()
        link_manifest = {
            "complete": True,
            "target_triple": object_manifest["target_triple"],
            "function_count": len(functions),
            "object_count": len(objects),
            "library": args.output.name,
            "library_size_bytes": len(linked_data),
            "library_sha256": hashlib.sha256(linked_data).hexdigest(),
            "dispatcher_entry": "ee_game_run",
            "dispatch_glue": dispatch_source.name,
        }
        manifest_path = args.output.with_suffix(args.output.suffix + ".link.json")
        manifest_path.write_text(json.dumps(link_manifest, indent=2) + "\n", encoding="utf-8")
        print(
            f"link_native_objects: PASS ({len(functions)} functions, "
            f"{len(linked_data):,} library bytes)",
            flush=True,
        )
        return 0

    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as exc:
        print(f"link_native_objects: ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
