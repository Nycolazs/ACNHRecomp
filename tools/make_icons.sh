#!/bin/bash
# The app's icon for the computers, from android/icon_512.png (tools/make_android_icon.py): assets/acnhrecomp.png
# (Linux), .ico (Windows, assets/acnhrecomp.rc), .icns (macOS, ACNHRecomp.app) and runtime/src/app_icon_rgba.inc
# (the windows' icon, app_icon.cpp). Needs Python with Pillow; the .icns needs macOS (iconutil).
cd "$(dirname "$0")/.." || exit 1
set -e
python3 - <<'PY'
from PIL import Image
im = Image.open('android/icon_512.png').convert('RGBA')
im.save('assets/acnhrecomp.png')
im.save('assets/acnhrecomp.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
px = im.resize((128, 128), Image.LANCZOS).tobytes()
with open('runtime/src/app_icon_rgba.inc', 'w') as f:
    f.write('// 128x128 RGBA, from assets/acnhrecomp.png (tools/make_icons.sh)\n')
    for i in range(0, len(px), 32):
        f.write(','.join(str(b) for b in px[i:i + 32]) + ',\n')
PY
if command -v iconutil >/dev/null; then
  set_dir=$(mktemp -d)/acnhrecomp.iconset
  mkdir -p "$set_dir"
  for z in 16 32 128 256 512; do
    sips -z $z $z android/icon_512.png --out "$set_dir/icon_${z}x${z}.png" >/dev/null
    [ $((z * 2)) -le 512 ] && sips -z $((z * 2)) $((z * 2)) android/icon_512.png --out "$set_dir/icon_${z}x${z}@2x.png" >/dev/null
  done
  iconutil -c icns "$set_dir" -o assets/acnhrecomp.icns
fi
ls -la assets runtime/src/app_icon_rgba.inc
