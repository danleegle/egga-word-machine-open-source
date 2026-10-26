"""Generate firmware font subsets from the included, openly licensed fonts."""
from pathlib import Path
import hashlib, json, subprocess

root = Path(__file__).resolve().parents[1]
fonts = root / "third_party/fonts"
converter = root / "node_modules/lv_font_conv/lv_font_conv.js"
if not converter.is_file():
    raise SystemExit("Install Node.js and run: pnpm install --frozen-lockfile")
for line in (fonts / "SHA256SUMS").read_text().splitlines():
    expected, name = line.split("  ", 1)
    if hashlib.sha256((fonts / name).read_bytes()).hexdigest() != expected:
        raise SystemExit("Font checksum mismatch: " + name)
for spec in json.loads((fonts / "font-specs.json").read_text(encoding="utf-8")):
    output = root / "main/components/78__xiaozhi-fonts/src" / (spec["name"] + ".c")
    args = ["node", str(converter), "--font", str(fonts / spec["font"]),
            "--range", "0x20-0x7E", "--size", str(spec["size"]), "--bpp", "1",
            "--format", "lvgl", "--lv-include", "lvgl.h", "--lv-font-name", spec["name"],
            "--no-compress", "--no-kerning", "--output", str(output)]
    if spec["symbols"]: args += ["--symbols", spec["symbols"]]
    if spec.get("symbol_font_chars"):
        args += ["--font", str(fonts / "NotoSansSymbols2-Regular.ttf"),
                 "--symbols", spec["symbol_font_chars"]]
    subprocess.run(args, check=True, cwd=root)
    text = output.read_text(encoding="utf-8")
    # Converter headers otherwise expose the developer's absolute home path.
    text = "\n".join(line for line in text.splitlines() if " * Opts:" not in line)
    output.write_text("// Generated from " + spec["font"] + "; see third_party/fonts.\n" + text + "\n", encoding="utf-8")
    print("Generated " + output.name)
