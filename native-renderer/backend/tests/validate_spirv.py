"""Extracts the SPIR-V arrays from the generated shader headers and runs
spirv-val on each (Vulkan 1.2 environment). Usage: validate_spirv.py <spirv-val> <dir>"""
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    spirv_val, folder = sys.argv[1], pathlib.Path(sys.argv[2])
    headers = sorted(folder.glob("*_spirv.h"))
    if not headers:
        print("no SPIR-V headers in", folder)
        return 1
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for h in headers:
            body = h.read_text().split("[] = {", 1)[1].split("};", 1)[0]
            data = bytes(int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", body))
            out = pathlib.Path(tmp) / (h.stem + ".spv")
            out.write_bytes(data)
            r = subprocess.run([spirv_val, "--target-env", "vulkan1.2", str(out)],
                               capture_output=True, text=True)
            status = "ok" if r.returncode == 0 else "FAILED"
            print(f"{h.name}: {len(data)} bytes, {status}")
            if r.returncode != 0:
                print(r.stdout, r.stderr)
                failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
