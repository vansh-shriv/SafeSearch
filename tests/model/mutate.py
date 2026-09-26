"""Mutation-test the model checker (python tests/model/mutate.py): inject defects into production sources, expect a VIOLATION each time."""
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.environ["PATH"] = r"C:\MinGW\bin;" + os.environ["PATH"]

MUTANTS = [
    ("floor ignored when checking a slot", "bootloader/src/boot_logic.c",
     "image_check_basic(slot_addr(slot), floor, h);", "image_check_basic(slot_addr(slot), floor & 0u, h);"),
    ("signature check skipped", "bootloader/src/boot_logic.c",
     "    if (s == IMG_OK)\n        s = image_check_signature(slot_addr(slot), h);\n", ""),
    ("trial counter never incremented", "bootloader/src/boot_logic.c",
     "        md.trial_count++;\n", "        md.trial_count += 0;\n"),
    ("metadata rewritten in place (no ping-pong)", "bootloader/src/metadata.c",
     "int target = newest < 0 ? 0 : 1 - newest;", "int target = 0;"),
    ("confirm resets the floor instead of raising it", "app/src/safeflash_app.c",
     "        md.min_allowed_version = h.version;", "        md.min_allowed_version = 0;"),
]


def run():
    r = subprocess.run(["mingw32-make", "-C", "tests/model", "check"], cwd=REPO, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


ok = True
for name, rel, old, new in MUTANTS:
    path = os.path.join(REPO, rel)
    orig = open(path, newline="").read()
    text = orig.replace("\r\n", "\n")
    if old not in text:
        print(f"SKIP  {name}: pattern not found in {rel}")
        ok = False
        continue
    open(path, "w", newline="\n").write(text.replace(old, new, 1))
    try:
        if os.path.exists(os.path.join(REPO, "tests/model/model_check.exe")):
            os.remove(os.path.join(REPO, "tests/model/model_check.exe"))
        rc, out = run()
        m = re.search(r"VIOLATION (\w+): (.*)", out)
        if rc != 0 and m:
            print(f"CAUGHT  {name}  ->  {m.group(1)}: {m.group(2)}")
        else:
            print(f"MISSED  {name}  (rc={rc})")
            ok = False
    finally:
        open(path, "w", newline="").write(orig)
sys.exit(0 if ok else 1)
