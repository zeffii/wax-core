#!/bin/sh
# Screen/cursor hand-off regression check for refactors of tools/wax_app.cpp: runs a set of key scripts headless and saves a screenshot of each.
#   tools/golden.sh save DIR [song.wax]      the pictures before the change
#   tools/golden.sh check DIR [song.wax]     the same run again; prints how many pixels of the content area differ from DIR (0..~50
#                                            from the blinking caret and the meters, a moved cursor or another pattern is thousands)
mode=$1; dir=$2; song=${3:-tests/songs/cloves.wax}
mkdir -p "$dir"
n=0
while IFS='|' read -r screen keys; do
  n=$((n + 1))
  frames=$((2 * $(echo "$keys" | tr ',' '\n' | wc -l) + 30))
  target="$dir/$n.bmp"; [ "$mode" = check ] && target="$dir/now$n.bmp"
  SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software ./build/wax-app --null --screen "$screen" --press "$keys" --screenshot "$target" --frames $frames "$song" >/dev/null 2>&1
  [ "$mode" = check ] && python3 -I -W ignore -c "
from PIL import Image, ImageChops
a=Image.open('$dir/$n.bmp').convert('RGB'); b=Image.open('$dir/now$n.bmp').convert('RGB')
w,h=a.size; box=(53,40,w,h-220)
d=ImageChops.difference(a.crop(box),b.crop(box)).convert('L').point(lambda v:255 if v>24 else 0)
print('$n $screen $keys:', sum(1 for v in d.getdata() if v))"
done <<'LIST'
song|f1
song|right,down,down,enter
song|down,down,enter,escape
song|down,down,down,enter,down,down,escape,down
song|right,right,down,ctrl+enter
song|down,down,enter,tab
song|down,enter,f3
song|down,enter,f3,f2
song|enter,down
pattern|down,down,down
pattern|enter,down,down,right
pattern|enter,escape,down
pattern|enter,c,down,d
instrument|f2
instrument|f4
LIST

true
