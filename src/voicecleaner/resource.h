#pragma once

// Icon resource IDs (see app.rc.in). The lowest-numbered ICON resource becomes the
// executable's default icon (taskbar / Explorer / Alt-Tab), so IDI_ACTIVE stays lowest.
// The tray swaps between all three at runtime based on UiState (see tray.cpp).
#define IDI_ACTIVE    101   // gray -> blue bars: cleaning on
#define IDI_BYPASSED  102   // all gray: bypass toggled (passthrough)
#define IDI_DOWN      103   // ghosted + red dot: worker down / device lost
