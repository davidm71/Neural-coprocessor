#!/usr/bin/env python3
"""PR1: make the Atomic MGPU Bridge release (production) source package.

Takes the committed tree (git archive HEAD), so uncommitted edits are never
shipped, and turns the diagnostic tree into the release one:

  - CMake option MGPU_PRODUCTION defaults to ON (quiet log, no developer
    probes in the panel; DetailedLog=1 still writes the full log).
  - assets/production/{mgpu.ini,README.txt,README.md} replace the shipped
    mgpu.ini, the install README.txt and the front-page README.md.
  - Development records, test logs and internal notes are left out.

Usage:  python3 tools/make_production.py <tag> [out_dir]
Writes  <out_dir>/Atomic-MGPU-Bridge-<tag>.zip with the same inner folder.
"""
import io, os, re, subprocess, sys, zipfile

DROP_PREFIX = ("assets/production/", "docs/", "history/", "reference/",
               "workarounds/", "tools/")
DROP_FILE = {"RESULTS.md", "METHOD.md", "ARCHITECTURE.md", "FORK_NOTES.md",
             "REPO_MANIFEST.md", "test-report-template.md"}
SWAP = {"assets/mgpu.ini": "assets/production/mgpu.ini",
        "assets/README.txt": "assets/production/README.txt",
        "README.md": "assets/production/README.md"}


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    tag = sys.argv[1]
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "."
    root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip()
    raw = subprocess.check_output(["git", "-C", root, "archive", "--format=zip", "HEAD"])
    src = zipfile.ZipFile(io.BytesIO(raw))
    files = {i.filename: src.read(i) for i in src.infolist() if not i.is_dir()}

    for dst, s in SWAP.items():
        files[dst] = files[s]

    cm = files["CMakeLists.txt"].decode()
    cm2 = cm.replace('option(MGPU_PRODUCTION "Release build: quiet log, no developer probes" OFF)',
                     'option(MGPU_PRODUCTION "Release build: quiet log, no developer probes" ON)')
    if cm2 == cm:
        sys.exit("MGPU_PRODUCTION option not found in CMakeLists.txt")
    files["CMakeLists.txt"] = cm2.encode()

    wf = ".github/workflows/build.yml"
    if wf in files:
        files[wf] = re.sub(rb"(upload-artifact@v4\s*\n\s*with:\s*\n\s*name:\s*)mgpu_bridge",
                           rb"\1atomic_mgpu_bridge", files[wf])

    keep = {n: b for n, b in files.items()
            if not n.startswith(DROP_PREFIX) and n not in DROP_FILE}

    name = f"Atomic-MGPU-Bridge-{tag}"
    out = os.path.join(out_dir, name + ".zip")
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for n in sorted(keep):
            z.writestr(f"{name}/{n}", keep[n])
    print(f"{out}: {len(keep)} files (dropped {len(files) - len(keep)})")


if __name__ == "__main__":
    main()
