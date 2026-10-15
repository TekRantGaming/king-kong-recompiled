"""Build SDK patch 0043 in a NEW copy using read-only -041 build inputs.

Run with Python312 -I under vcvars64 (build-hdr-127.ps1 does that). No shared
SDK objects, sources, install headers or binaries are overwritten.
"""
from pathlib import Path
import difflib
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
base = Path(r"F:\KK-native-renderer\sdk-src-upscalers-041")
source = Path(r"F:\KK-native-renderer\sdk-src-upscalers-043")
install_base = Path(r"F:\KK-native-renderer\sdk-vulkan-upscalers-041")
install = Path(r"F:\KK-native-renderer\sdk-vulkan-upscalers-043")
build = base / "out/build/win-amd64"
out = root / "kk/out/hdr-sdk-127"
out.mkdir(parents=True, exist_ok=True)
cache = (build / "CMakeCache.txt").read_text()
ninja = re.search(r"^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$", cache, re.M)[1]
commands = subprocess.check_output([ninja, "-f", "build-Release.ninja", "-t", "commands", "rexruntime"], cwd=build, text=True).splitlines()

# Embed both backends' bytecode from the same production shader.
dxc = Path(r"F:\KK-native-renderer\downloads\dxc-v1.9.2609\dxc\bin\x64\dxc.exe")
bytecode = source / "src/ui/shaders/bytecode"
bytecode.mkdir(parents=True, exist_ok=True)
for api in ("d3d12", "vulkan"):
    for stage in ("vs", "ps"):
        name = f"hdr_{api}_{stage}"
        args = [str(dxc), "-T", stage + "_6_0", "-E", stage + "main", "-O3",
                str(source / "src/ui/shaders/guest_output_hdr.hlsl")]
        if api == "vulkan":
            blob = out / (name + ".spv")
            args += ["-spirv", "-fspv-target-env=vulkan1.1", "-D", "VULKAN", "-Fo", str(blob)]
        else:
            args += ["-Fh", str(bytecode / (name + ".h")), "-Vn", name, "-Qstrip_debug", "-Qstrip_reflect"]
        subprocess.run(args, check=True)
        if api == "vulkan":
            import struct
            words = struct.unpack("<" + "I" * (blob.stat().st_size // 4), blob.read_bytes())
            (bytecode / (name + ".inc")).write_text("\n".join(
                ", ".join(f"0x{x:08x}" for x in words[i:i+8]) + "," for i in range(0, len(words), 8)) + "\n")
            subprocess.run([r"F:\KK-native-renderer\downloads\vulkan-sdk-latest\sdk\Bin\spirv-val.exe", "--target-env", "vulkan1.1", str(blob)], check=True)

files = ["src/ui/presenter.cpp", "src/ui/d3d12/d3d12_presenter.cpp",
         "src/ui/d3d12/d3d12_immediate_drawer.cpp", "src/ui/d3d12/d3d12_provider.cpp",
         "src/ui/vulkan/vulkan_presenter.cpp", "src/ui/vulkan/vulkan_immediate_drawer.cpp",
         "src/ui/vulkan/vulkan_instance.cpp", "src/ui/vulkan/vulkan_provider.cpp"]
objects = {}
for file in files:
    cmd = next(c for c in commands if c.endswith(" -c " + (base / file).as_posix()))
    old_obj = re.search(r" -o (\S+) -c ", cmd)[1]
    obj = (out / (Path(file).name + ".obj")).as_posix()
    objects[old_obj] = obj
    cmd = cmd.replace(" -o " + old_obj + " ", " -o " + obj + " ")
    cmd = re.sub(r" -MD -MT .*? -MF .*? -o ", " -o ", cmd)
    cmd = re.sub(r" -Xclang -include-pch -Xclang \S+", "", cmd)
    cmd = re.sub(r" -Xclang -include -Xclang \S+", "", cmd)
    cmd = cmd[:cmd.index(" -I")] + " -I" + (source / "include").as_posix() + " -I" + (source / "src").as_posix() + " -I" + (base / file).parent.as_posix() + cmd[cmd.index(" -I"):]
    if (source / file).exists():
        cmd = cmd.replace("-c " + (base / file).as_posix(), "-c " + (source / file).as_posix())
    with (out / (Path(file).name + ".log")).open("w") as log:
        result = subprocess.run(cmd, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / (Path(file).name + ".log")).read_text())
        raise SystemExit(result.returncode)
    print("Compiled", file, flush=True)

def replace_objects(text):
    for old, new in objects.items():
        text = text.replace(old, new).replace(old.replace("/", "\\"), new)
    return text

exports = out / "exports.def.objs"
exports.write_text(replace_objects((build / "src/system/CMakeFiles/rexruntime.dir/Release/exports.def.objs").read_text()))
compiler = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang++.exe")
subprocess.run(["cmake", "-E", "__create_def", str(out / "exports.def"), str(exports),
                "--nm=" + str(compiler.parent / "llvm-nm.exe")], cwd=build, check=True)
block = next(b for b in (build / "CMakeFiles/impl-Release.ninja").read_text().split("\n\n")
             if ": CXX_SHARED_LIBRARY_LINKER__rexruntime_Release " in b)
inputs = block.split(": CXX_SHARED_LIBRARY_LINKER__rexruntime_Release ", 1)[1].split(" |", 1)[0]
libs = re.search(r"^  LINK_LIBRARIES = (.*)$", block, re.M)[1]
(out / "runtime.rsp").write_text(replace_objects(inputs) + " " + libs)
subprocess.run([str(compiler), "-nostartfiles", "-nostdlib", "-march=x86-64-v2", "-O3", "-DNDEBUG", "-D_DLL", "-D_MT",
               "-Xclang", "--dependent-lib=msvcrt", "-shared", "-fuse-ld=lld", "-Xlinker", "/DEF:" + str(out / "exports.def"),
               "-o", str(out / "rexruntime.dll"), "-Xlinker", "/MANIFEST:EMBED", "-Xlinker", "/implib:" + str(out / "rexruntime.lib"),
               "-Xlinker", "/pdb:" + str(out / "rexruntime.pdb"), "@" + str(out / "runtime.rsp")], cwd=build, check=True)

if not install.exists():
    shutil.copytree(install_base, install, ignore=shutil.ignore_patterns("*.pdb", "*.ilk"))
for ext, sub in (("dll", "bin"), ("lib", "lib")):
    shutil.copy2(out / ("rexruntime." + ext), install / "win-amd64" / sub / ("rexruntime." + ext))
for file in ("rex/ui/hdr.h", "rex/ui/vulkan/presenter.h", "rex/ui/d3d12/d3d12_presenter.h"):
    shutil.copy2(source / "include" / file, install / "win-amd64/include" / file)

# A normal SDK rebuild can apply this patch without task-local paths/scripts.
changed = ["include/rex/ui/hdr.h", "include/rex/ui/d3d12/d3d12_presenter.h", "include/rex/ui/vulkan/presenter.h"]
changed += [f for f in files if (source / f).exists()]
changed += ["src/ui/d3d12/hdr_127.inc", "src/ui/vulkan/hdr_127.inc", "src/ui/shaders/guest_output_hdr.hlsl"]
changed += [p.relative_to(source).as_posix() for p in sorted(bytecode.iterdir()) if p.name.startswith("hdr_")]
patch = ""
for file in changed:
    original = (base / file).read_text(encoding="utf-8") if (base / file).exists() else ""
    fixed = (source / file).read_text(encoding="utf-8")
    if original == fixed:
        continue
    patch += "".join(difflib.unified_diff(original.splitlines(True), fixed.splitlines(True),
                fromfile="a/" + file if original else "/dev/null", tofile="b/" + file))
(root / "native-renderer/sdk-patches/0043-scrgb-hdr-output.patch").write_text(patch, encoding="utf-8", newline="\n")
(install / "hdr-sdk-127.txt").write_text("SDK -041 plus native-renderer/sdk-patches/0043-scrgb-hdr-output.patch\n")
print("Built isolated SDK", install)
