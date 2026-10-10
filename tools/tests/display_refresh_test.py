"""Keep display redraw areas aligned to the panel's drawing windows."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from controller_serialization_test import function

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/hardware/display_manager.cpp").read_text()

HARNESS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
struct lv_area_t { int32_t x1, y1, x2, y2; };
struct lv_event_t { lv_area_t* area; };
void* lv_event_get_param(lv_event_t* event) { return event->area; }

class DisplayManager {
public:
    uint32_t screen_width = 280;
    uint32_t screen_height = 456;
    static void display_rounder_cb(lv_event_t* e);
};
DisplayManager display;
DisplayManager* g_display_manager = &display;
''' + function(SOURCE, "void DisplayManager::display_rounder_cb(lv_event_t* e)") + r'''

lv_area_t round_area(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    lv_area_t area{x1, y1, x2, y2};
    lv_event_t event{&area};
    DisplayManager::display_rounder_cb(&event);
    return area;
}

int main() {
    // The SH8601 needs windows on 2-pixel boundaries: start on an even row,
    // end on an odd one, always across the full width.
    const lv_area_t label = round_area(40, 197, 230, 252);  // the centred weight label
    assert(label.x1 == 0 && label.x2 == 279 && label.y1 == 196 && label.y2 == 253);
    const lv_area_t aligned = round_area(0, 100, 279, 115);
    assert(aligned.y1 == 100 && aligned.y2 == 115);
    const lv_area_t last_row = round_area(10, 455, 20, 455);
    assert(last_row.y1 == 454 && last_row.y2 == 455);
    const lv_area_t first_row = round_area(5, 0, 6, 0);
    assert(first_row.y1 == 0 && first_row.y2 == 1);
}
'''


class DisplayRefreshTest(unittest.TestCase):
    def test_redraw_areas_are_panel_aligned(self):
        with tempfile.TemporaryDirectory() as folder:
            cpp, binary = Path(folder) / "rounder.cpp", Path(folder) / "rounder"
            cpp.write_text(HARNESS)
            subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
