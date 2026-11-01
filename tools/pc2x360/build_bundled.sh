#!/usr/bin/env bash
# Convert, pack and validate the bundled add-on cars (one line per car).
# usage: build_bundled.sh <export_dir> <work_dir> <sources_dir> <out_dir>
#   export_dir   stock 360 cars exported by the app (logs/export)
#   work_dir     their decoded bodies (xlzx unrsc <car>.xft <work>/<car>.xft.bin, same for .xtd)
#   sources_dir  the mods' PC files: <sources>/<model>/<model>.wft and .wtd
#   out_dir      gets <model>/<model>.xft/.xtd ready for addons/<model>/
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export_dir=$1 work=$2 sources=$3 out=$4
fails=0
# model         template  pivot (pop-up lamps the trainer turns)
while read -r model template pivot; do
  [ -z "$model" ] && continue
  [ "$pivot" = "-" ] && pivot=""
  o="$out/$model"
  mkdir -p "$o"
  python3 "$here/run_vehicle.py" "$export_dir" "$work" "$sources/$model/$model.wft" "$sources/$model/$model.wtd" \
    "$o" "$model" "$template" ${pivot:+"$pivot"} | sed "s/^/[$model] /"
  while read -r f type flags; do
    "$here/xlzx" rsc "$type" "$flags" "$o/$f.bin" "$o/$f" >/dev/null
  done < "$o/flags.txt"
  if ! python3 "$here/validate_vehicle.py" "$o" "$model" "$sources/$model/$model.wft" "$sources/$model/$model.wtd" \
      ${pivot:+"$pivot"} > "$o/validate.txt"; then
    fails=$((fails + 1))
  fi
  grep -E "FAIL|FAILURES" "$o/validate.txt" | sed "s/^/[$model] /"
done <<'CARS'
batmobile  fbi      -
f16        fbi      -
deimos     banshee  extra_1=misc_a,misc_y,misc_w,misc_p,misc_m,misc_headlight_l,misc_headlight_r
dom645t    banshee  -
CARS
exit $fails
