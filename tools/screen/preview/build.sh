#!/bin/bash
# build.sh OUTDIR: compile LVGL + the status card for the PC and draw every state into OUTDIR
# (one PNG each, sheet.png with all of them at 2x). LVGL comes from ~/Arduino/libraries/lvgl
# (LVGL=dir to use another copy); objects are cached in OUTDIR/obj.
set -e
OUT=${1:?outdir}; mkdir -p "$OUT/obj"
HERE=$(cd "$(dirname "$0")" && pwd); SK=$HERE/../../../firmware/JcorpNomadProject
LVGL=${LVGL:-$HOME/Arduino/libraries/lvgl}
CF="-O1 -w -DLV_CONF_INCLUDE_SIMPLE -I$SK -I$LVGL/src -I$LVGL"
objs=()
while IFS= read -r c; do
  o="$OUT/obj/$(echo "$c" | sed "s|$LVGL/||; s|/|_|g").o"
  [ "$o" -nt "$c" ] || gcc $CF -c "$c" -o "$o"
  objs+=("$o")
done < <(find "$LVGL/src" -name '*.c')
gcc $CF -c "$SK/nomad_status_fonts.c" -o "$OUT/obj/fonts.o"
gcc $CF -c "$SK/nomad_status_icons.c" -o "$OUT/obj/icons.o"
gcc $CF -c "$SK/ui_img_1859342711.c" -o "$OUT/obj/logo.o"
g++ $CF -c "$SK/NomadStatus.cpp" -o "$OUT/obj/status.o"
g++ $CF -c "$HERE/preview.cpp" -o "$OUT/obj/preview.o"
g++ -o "$OUT/preview" "$OUT/obj/preview.o" "$OUT/obj/status.o" "$OUT/obj/fonts.o" "$OUT/obj/icons.o" "$OUT/obj/logo.o" "${objs[@]}" -lm
(cd "$OUT" && ./preview)
python3 - "$OUT" <<'EOF'
import sys, glob, os
from PIL import Image
out = sys.argv[1]
files = sorted(glob.glob(os.path.join(out, "*.ppm")))
ims = []
for f in files:
    im = Image.open(f); png = f[:-4] + ".png"; im.save(png); os.remove(f); ims.append(im)
cols = 6; rows = (len(ims) + cols - 1) // cols; gap = 12
sheet = Image.new("RGB", (cols * (172 + gap) + gap, rows * (320 + gap) + gap), (40, 40, 40))
for i, im in enumerate(ims):
    sheet.paste(im, (gap + (i % cols) * (172 + gap), gap + (i // cols) * (320 + gap)))
sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
sheet.save(os.path.join(out, "sheet.png"))
print(len(ims), "cards ->", os.path.join(out, "sheet.png"))
EOF
