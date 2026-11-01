"""Copies the glass menu's drawing (namespace glass ... DrawGlassMenu) out of
src/gta4_trainer.cpp into glass_snippet.inc, for glass_preview.cpp.

usage: python3 extract_glass.py <menu src>/gta4_trainer.cpp <out dir>
then:  g++ -std=c++20 -O2 -I<imgui> -I<out dir> glass_preview.cpp <imgui>/imgui*.cpp -o glass_preview
       ./glass_preview background.ppm out.ppm <page 0-2> <seconds into the page>
(background: a 1920x1080 binary PPM, e.g. a game frame; the fonts are the hub's,
from the port's ps5/assets/fonts)."""
import os
import sys

src = open(sys.argv[1]).read()
a = src.index('namespace glass {')
b = src.index('class TrainerOverlay final : public rex::ui::ImGuiDialog {')
open(os.path.join(sys.argv[2], 'glass_snippet.inc'), 'w').write(src[a:b])
