#!/usr/bin/env python3
"""Platform-independent command tests for the Windows native linker path."""

import importlib.util
import sys
from pathlib import Path


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_windows_link_commands.py source_root")
    source_root = Path(sys.argv[1]).resolve()
    script = source_root / "tools" / "link_native_objects.py"
    spec = importlib.util.spec_from_file_location("native_linker", script)
    linker = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(linker)

    available = {"cl.exe", "clang++"}
    which = lambda name: name if name in available else None
    require(
        linker.choose_compiler(None, platform="win32", which=which) == "cl.exe",
        "Windows default must prefer Visual Studio cl.exe",
    )
    require(
        linker.choose_compiler(None, pdb=True, platform="win32", which=which)
        == "clang++",
        "PDB mode must retain its Clang/LLD compiler",
    )
    require(
        linker.choose_compiler("custom++", platform="win32", which=which)
        == "custom++",
        "Explicit CXX or --cxx choice must be preserved",
    )
    require(
        linker.choose_compiler(None, platform="linux", which=which) == "c++",
        "Non-Windows compiler choice must not change",
    )
    require(
        linker.is_msvc_compiler(r"C:\Visual Studio\VC\bin\cl.exe")
        or linker.is_msvc_compiler("cl.exe"),
        "cl.exe must be recognized as an MSVC compiler",
    )
    require(
        not linker.is_msvc_compiler("clang++"),
        "Clang GNU-style command lines must remain unchanged",
    )
    try:
        linker.choose_compiler(None, platform="win32", which=lambda name: None)
    except ValueError as exc:
        require("compiler" in str(exc), "missing compiler diagnostic must be useful")
    else:
        raise AssertionError("a missing compiler must fail clearly")

    # A space-containing source root is deliberately used here to
    # ensure all paths stay individual subprocess arguments.
    includes = [
        Path(r"C:\Dev Folder\repo\include"),
        Path(r"C:\Dev Folder\repo\include\ee"),
        Path(r"C:\Dev Folder\repo\src\runtime"),
    ]
    dispatch = Path(r"C:\Build Folder\game.dll.dispatch.cpp")
    object_file = Path(r"C:\Build Folder\game.dll.dispatch.obj")
    compile_command = linker.msvc_compile_command(
        "cl.exe", [dispatch], object_file, includes, False,
    )
    require(compile_command[0] == "cl.exe", "MSVC compiler mismatch")
    for flag in ("/nologo", "/std:c++17", "/EHsc", "/MD",
                 "/bigobj", "/Od", "/c"):
        require(flag in compile_command, f"missing MSVC flag {flag}")
    require("-std=c++17" not in compile_command, "GNU flag passed to cl.exe")
    require("-o" not in compile_command, "GNU output flag passed to cl.exe")
    require("/Fo" + str(object_file) in compile_command,
            "dispatch object output destination is incorrect")
    for include in includes:
        require("/I" + str(include) in compile_command,
                "missing include argument")
    require(str(dispatch) in compile_command, "missing glue source")
    optimized = linker.msvc_compile_command(
        "cl.exe", [dispatch], object_file, includes, True,
    )
    require("/O2" in optimized and "/Od" not in optimized,
            "MSVC optimized runtime compiler flags are incorrect")

    response_file = Path(r"C:\Build Folder\game.dll.objects.rsp")
    runtime_objects = [
        Path(r"C:\Build Folder\game.dll.ee_memory.obj"),
        Path(r"C:\Build Folder\game.dll.ee_dispatcher.obj"),
        object_file,
    ]
    linked = linker.msvc_link_command(
        "link.exe", Path(r"C:\Build Folder\game.dll"),
        response_file, runtime_objects,
    )
    require(linked[:4] == ["link.exe", "/NOLOGO", "/DLL", "/MACHINE:X64"],
            "MSVC linker flags are incorrect")
    require("/OUT:C:\\Build Folder\\game.dll" in linked,
            "DLL output location is incorrect")
    require("@"+str(response_file) in linked,
            "LLVM COFF object response file missing")
    for obj in runtime_objects:
        require(str(obj) in linked, "compiled runtime object missing")
    require("-shared" not in linked, "GNU linker flag passed to link.exe")

    print("Windows native MSVC compiler/linker command selection: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
