import sys
from pathlib import Path

ORIGINAL = Path(__file__).resolve().parents[2] / "source" / "native" / "generate_build_inputs.py"

REPLACEMENTS = [
    ("if digest(glslang) != '8F0838D3FD981D4086D81675C4CFA4381691AEAFC3651D12D62F61EF0CEBB33E':\n"
     "    raise RuntimeError('Expected glslangValidator from Vulkan SDK 1.2.176.1')\n",
     ""),
    ("    subprocess.run([str(glslang), '-V', f'-DMFG_COLOR={mode}',\n"
     "                    '-o', str(spv), str(source_dir / 'ui_color.frag')], check=True)\n",
     "    shutil.copyfile(glslang / f'ui_color_{mode}.spv', spv)\n"),
]


def patched_source():
    text = ORIGINAL.read_text(encoding="utf-8")
    for old, new in REPLACEMENTS:
        if text.count(old) != 1:
            raise SystemExit(f"{ORIGINAL} no longer contains the text this wrapper replaces:\n{old}")
        text = text.replace(old, new)
    return text


def main():
    sys.argv = [str(ORIGINAL), *sys.argv[1:]]
    exec(compile(patched_source(), str(ORIGINAL), "exec"), {"__name__": "__main__", "__file__": str(ORIGINAL)})


main()
