"""Runs a test executable and fails if the Khronos validation layer (enabled
through VK_INSTANCE_LAYERS) printed any error. Usage: check_validation.py <exe> [args]"""
import subprocess
import sys


def main():
    r = subprocess.run(sys.argv[1:], capture_output=True, text=True)
    out = r.stdout + r.stderr
    sys.stdout.write(out)
    errors = [l for l in out.splitlines() if "Validation Error" in l]
    if errors:
        print(f"\n{len(errors)} Vulkan validation error(s)")
        return 1
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
