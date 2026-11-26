// On-device LCD feedback (spec section 8).
#pragma once

namespace lcdui {

// Prepares the display. Safe to call on boards without a screen.
void begin();

// Refreshes the screen (throttled internally). Call from loop().
void loop();

}  // namespace lcdui
