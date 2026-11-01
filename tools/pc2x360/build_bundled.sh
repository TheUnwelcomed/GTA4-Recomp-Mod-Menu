#!/usr/bin/env bash
# Convert, pack and validate the bundled add-on cars (one line per car).
# usage: build_bundled.sh <export_dir> <work_dir> <sources_dir> <out_dir> [model ...]
#   export_dir   stock 360 cars exported by the app (logs/export)
#   work_dir     their decoded bodies (xlzx unrsc <car>.xft <work>/<car>.xft.bin, same for .xtd)
#   sources_dir  the mods' PC files: <sources>/<model>/<model>.wft and .wtd (or, for
#                cars sharing a dictionary, the ones named in the line's last column)
#   out_dir      gets <model>/<model>.xft/.xtd ready for addons/<model>/
#   model ...    only these cars (default: all)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
export_dir=$1 work=$2 sources=$3 out=$4
shift 4
only=" $* "
fails=0
# model  template  pivots the trainer turns (canopies, pop-up lamps, landing gear)  shaders from other cars
#   [texture dictionaries, under sources_dir, the car's own first: when it shares a parent's]
#   [options: batconv.parse_options, e.g. lods=0:0:2,tex=512,detail=256 for a heavy model]
while read -r model template pivot shaders wtds options; do
  [ -z "$model" ] && continue
  [ "$only" != "  " ] && [[ "$only" != *" $model "* ]] && continue
  [ "$pivot" = "-" ] && pivot=""
  wtd="$sources/$model/$model.wtd"
  if [ -n "${wtds:-}" ] && [ "$wtds" != "-" ]; then
    wtd=""
    IFS=',' read -ra parts <<< "$wtds"
    for p in "${parts[@]}"; do wtd="${wtd:+$wtd,}$sources/$p"; done
  fi
  o="$out/$model"
  mkdir -p "$o"
  python3 "$here/run_vehicle.py" "$export_dir" "$work" "$sources/$model/$model.wft" "$wtd" \
    "$o" "$model" "$template" "${pivot:--}" "${shaders:--}" "${options:--}" | sed "s/^/[$model] /"
  while read -r f type flags; do
    "$here/xlzx" rsc "$type" "$flags" "$o/$f.bin" "$o/$f" >/dev/null
  done < "$o/flags.txt"
  if ! python3 "$here/validate_vehicle.py" "$o" "$model" "$sources/$model/$model.wft" "$wtd" \
      "${pivot:--}" "${options:--}" > "$o/validate.txt"; then
    fails=$((fails + 1))
  fi
  grep -E "FAIL|FAILURES" "$o/validate.txt" | sed "s/^/[$model] /"
done <<'CARS'
batmobile  fbi      interiorlight@0,-0.44,0.38=door_pside_f,windscreen  -
f16        cavalcade  extra_1@0,2.73,-0.22=boot:y>1.5;extra_5@-0.658,-0.073,-0.028=boot:y<=1.5;extra_6@0.651,-0.073,-0.027=door_pside_f;interiorlight@0,2.3,1.22=windscreen  airtug,fbi
deimos     banshee  extra_1=misc_a,misc_y,misc_w,misc_p,misc_m,misc_headlight_l,misc_headlight_r  -
dom645t    banshee  -  -
nexus      banshee  extra_1=light_cover;overheat=door_dside_f;overheat_2=door_pside_f  -
arbiterx   banshee  -  -
lazar3     cavalcade  extra_1=turret_1base;extra_5=turret_1barrel;extra_6=door_dside_f;interiorlight=door_pside_f  -
gauntletc  banshee  -  -
delnor     banshee  overheat=door_dside_f;overheat_2=door_pside_f  -  delnor/delnor.wtd,bttf/vehshare_bttf.wtd  lods=0:0:2,tex=512,detail=256
delone     banshee  overheat=door_dside_f;overheat_2=door_pside_f  -  bttf/delorean.wtd,bttf/vehshare_bttf.wtd  lods=0:0:2,tex=512,detail=256
deltwo     banshee  overheat=door_dside_f;overheat_2=door_pside_f  -  bttf/delorean.wtd,bttf/vehshare_bttf.wtd  lods=0:0:2,tex=512,detail=256
delthr     banshee  overheat=door_dside_f;overheat_2=door_pside_f  -  bttf/delorean.wtd,bttf/vehshare_bttf.wtd  lods=0:0:2,tex=512,detail=256
delhov     banshee  overheat=door_dside_f;overheat_2=door_pside_f  -  bttf/delorean.wtd,bttf/vehshare_bttf.wtd  lods=0:0:2,tex=512,detail=256
CARS
exit $fails
