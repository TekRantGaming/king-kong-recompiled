"""Build the presenter fix into a private DLL, reusing existing SDK objects.

Run under vcvars64 with the mandated Python312 -I. Never edits the shared SDK.
The nested framebuffer type changes, but the presenter object's size does not.
"""
import difflib
from pathlib import Path
import re
import subprocess
import sys
import shutil
import os

sdk = Path(r"F:\KK-native-renderer\sdk-src-upscalers")
build = sdk / "out/build/win-amd64"
workspace = Path(__file__).resolve().parents[2]
dest = Path(sys.argv[1]).resolve()
install = Path(r"F:\KK-native-renderer\sdk-vulkan-upscalers-035")
assert dest.is_relative_to(Path(r"F:\KK-native-renderer\runs")) or dest == install
if dest == install:
    assert not dest.exists(), "Refuse to overwrite an existing isolated SDK"
    shutil.copytree(Path(r"F:\KK-native-renderer\sdk-vulkan-upscalers"), dest)
dest.mkdir(parents=True, exist_ok=True)


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


patch = ""
for name in ["include/rex/ui/vulkan/presenter.h", "src/ui/vulkan/vulkan_presenter.cpp"]:
    original = (sdk / name).read_text()
    fixed = original
    if name.endswith(".h"):
        fixed = replace_once(fixed,
            "SwapchainFramebuffer(VkImageView image_view, VkFramebuffer framebuffer)\n"
            "          : image_view(image_view), framebuffer(framebuffer) {}",
            "SwapchainFramebuffer(VkImageView image_view, VkFramebuffer framebuffer, VkSemaphore present_semaphore)\n"
            "          : image_view(image_view), framebuffer(framebuffer), present_semaphore(present_semaphore) {}")
        fixed = replace_once(fixed, "      VkFramebuffer framebuffer;\n    };",
            "      VkFramebuffer framebuffer;\n      VkSemaphore present_semaphore;\n    };")
    else:
        fixed = replace_once(fixed,
            "  VkSemaphore present_semaphore = paint_submission.present_semaphore();",
            "  // Reacquiring this image proves its previous presentation wait has finished.\n"
            "  // A graphics submission fence alone cannot prove that for a different image.\n"
            "  VkSemaphore present_semaphore = paint_context_.swapchain_framebuffers[swapchain_image_index].present_semaphore;")
        fixed = replace_once(fixed,
            "    paint_context_.swapchain_framebuffers.emplace_back(image_view, framebuffer);",
            "    VkSemaphoreCreateInfo semaphore_info{};\n"
            "    semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;\n"
            "    VkSemaphore present_semaphore;\n"
            "    if (dfn.vkCreateSemaphore(device, &semaphore_info, nullptr, &present_semaphore) != VK_SUCCESS) {\n"
            "      dfn.vkDestroyFramebuffer(device, framebuffer, nullptr);\n"
            "      dfn.vkDestroyImageView(device, image_view, nullptr);\n"
            "      paint_context_.DestroySwapchainAndVulkanSurface();\n"
            "      return SurfacePaintConnectResult::kFailure;\n"
            "    }\n"
            "    paint_context_.swapchain_framebuffers.emplace_back(image_view, framebuffer, present_semaphore);")
        fixed = replace_once(fixed,
            "  for (const SwapchainFramebuffer& framebuffer : swapchain_framebuffers) {",
            "  // Drain presentation waits before destroying their per-image semaphores.\n"
            "  if (!swapchain_framebuffers.empty()) {\n"
            "    auto queue = vulkan_device->AcquireQueue(present_queue_family, 0);\n"
            "    const auto* instance = vulkan_device->vulkan_instance();\n"
            "    auto wait_idle = reinterpret_cast<PFN_vkQueueWaitIdle>(instance->functions().vkGetInstanceProcAddr(\n"
            "        instance->instance(), \"vkQueueWaitIdle\"));\n"
            "    if (wait_idle) wait_idle(queue.queue());\n"
            "  }\n"
            "  for (const SwapchainFramebuffer& framebuffer : swapchain_framebuffers) {\n"
            "    dfn.vkDestroySemaphore(device, framebuffer.present_semaphore, nullptr);")
    target = dest / name
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(fixed)
    patch += "".join(difflib.unified_diff(original.splitlines(True), fixed.splitlines(True),
        fromfile="a/" + name, tofile="b/" + name))
(workspace / "native-renderer/sdk-patches/0035-present-semaphores-per-image.patch").write_text(patch)

cache = (build / "CMakeCache.txt").read_text()
ninja = re.search(r"^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$", cache, re.M).group(1)
obj = "src/ui/CMakeFiles/rexui.dir/Release/vulkan/vulkan_presenter.cpp.obj"


def commands(target):
    return subprocess.check_output([ninja, "-f", "build-Release.ninja", "-t", "commands", target],
        cwd=build, text=True).splitlines()[-1]


compile_cmd = commands(obj)
compile_cmd = compile_cmd.replace("-I" + sdk.as_posix() + "/include",
    "-I" + (dest / "include").as_posix() + " -I" + sdk.as_posix() + "/src/ui/vulkan -I" + sdk.as_posix() + "/include")
compile_cmd = compile_cmd.replace((sdk / "src/ui/vulkan/vulkan_presenter.cpp").as_posix(),
    (dest / "src/ui/vulkan/vulkan_presenter.cpp").as_posix())
compile_cmd = compile_cmd.replace(obj.replace("/", "\\") + ".d", (dest / "presenter.obj.d").as_posix())
compile_cmd = compile_cmd.replace(obj, (dest / "presenter.obj").as_posix())
(dest / "compile-command.txt").write_text(compile_cmd)
subprocess.run(compile_cmd, cwd=build, check=True)

# Extract existing inputs without running Ninja, which would write to the shared SDK.
# Generate the changed private-type exports into our private build directory too.
export_objs = (build / "src/system/CMakeFiles/rexruntime.dir/Release/exports.def.objs").read_text()
export_objs = export_objs.replace(obj, (dest / "presenter.obj").as_posix())
export_objs = export_objs.replace(obj.replace("/", "\\"), str(dest / "presenter.obj"))
(dest / "exports.def.objs").write_text(export_objs)
compiler = re.search(r"^CMAKE_CXX_COMPILER:STRING=(.+)$", cache, re.M).group(1)
subprocess.run([r"C:\Program Files\CMake\bin\cmake.exe", "-E", "__create_def",
    str(dest / "exports.def"), str(dest / "exports.def.objs"),
    "--nm=" + str(Path(compiler).parent / "llvm-nm.exe")], cwd=build, check=True)
ninja_text = (build / "CMakeFiles/impl-Release.ninja").read_text()
block = next(b for b in ninja_text.split("\n\n") if ": CXX_SHARED_LIBRARY_LINKER__rexruntime_Release " in b)
inputs = block.split(": CXX_SHARED_LIBRARY_LINKER__rexruntime_Release ", 1)[1].split(" |", 1)[0]
libraries = re.search(r"^  LINK_LIBRARIES = (.*)$", block, re.M).group(1)
rsp = dest / "runtime.rsp"
rsp.write_text(inputs.replace(obj, (dest / "presenter.obj").as_posix()) + " " + libraries)
link_cmd = commands("rexruntime:Release").split(" && ", 1)[1].rsplit(" && ", 1)[0]
for suffix in ["dll", "lib", "pdb"]:
    link_cmd = link_cmd.replace(str(sdk / f"out/win-amd64/Release/rexruntime.{suffix}"),
        str(dest / f"rexruntime.{suffix}"))
link_cmd = link_cmd.replace("@CMakeFiles\\rexruntime.Release.rsp", "@" + str(rsp))
link_cmd = link_cmd.replace("/DEF:src\\system\\CMakeFiles\\rexruntime.dir\\Release\\exports.def",
    "/DEF:" + str(dest / "exports.def"))
(dest / "link-command.txt").write_text(link_cmd)
subprocess.run(link_cmd, cwd=build, check=True)
print(dest / "rexruntime.dll")
if dest == install:
    for suffix in ["dll", "pdb"]:
        if (dest / f"rexruntime.{suffix}").exists():
            shutil.copy2(dest / f"rexruntime.{suffix}", dest / "win-amd64/bin" / f"rexruntime.{suffix}")
    shutil.copy2(dest / "rexruntime.lib", dest / "win-amd64/lib/rexruntime.lib")
    shutil.copy2(dest / "include/rex/ui/vulkan/presenter.h", dest / "win-amd64/include/rex/ui/vulkan/presenter.h")
    # The AMD patch affects compiled GLSL, so copying the original signed DLL
    # would silently discard it. Rebuild only inside the isolated SDK copy.
    ffx = dest / "fidelityfx"
    shutil.copytree(Path(r"F:\KK-native-renderer\downloads\amd-fidelityfx-sdk-v1.1.4\x"), ffx)
    subprocess.run(["git", "apply", "--ignore-space-change", str(workspace / "native-renderer/sdk-patches/0036-amd-fsr-luma-history-format.patch")], cwd=ffx, check=True)
    env = dict(os.environ, VULKAN_SDK=r"F:\KK-native-renderer\downloads\vulkan-sdk-latest\sdk")
    subprocess.run(["cmake", "-S", str(ffx / "ffx-api"), "-B", str(dest / "fidelityfx-build"),
                    "-G", "Visual Studio 17 2022", "-A", "x64", "-DFFX_API_BACKEND=VK_X64"], env=env, check=True)
    subprocess.run(["cmake", "--build", str(dest / "fidelityfx-build"), "--config", "Release", "--parallel", "4"], env=env, check=True)
    shutil.copy2(ffx / "ffx-api/bin/amd_fidelityfx_vk.dll", ffx / "PrebuiltSignedDLL/amd_fidelityfx_vk.dll")
