"""Strip Clang-only -Wno-* flags from generated ninja files so MSVC (is_clang=false) builds succeed."""
import os, re, glob

OUT = os.path.join(os.path.dirname(__file__), "out", "x64")

# Match -Wno-<anything> as a standalone token (space-separated)
CLANG_FLAG = re.compile(r'\s-Wno-[A-Za-z0-9_-]+')

count = 0
for path in glob.glob(os.path.join(OUT, "**", "*.ninja"), recursive=True):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    cleaned = CLANG_FLAG.sub("", text)
    if cleaned != text:
        with open(path, "w", encoding="utf-8") as f:
            f.write(cleaned)
        count += 1

# Also handle the top-level build.ninja
top = os.path.join(OUT, "build.ninja")
if os.path.isfile(top):
    with open(top, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    cleaned = CLANG_FLAG.sub("", text)
    if cleaned != text:
        with open(top, "w", encoding="utf-8") as f:
            f.write(cleaned)
        count += 1

print(f"Patched {count} ninja file(s)")
