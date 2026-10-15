// The game's view (app_view.mm): OS2View, the MTKView that keeps the camera
// and the keys held down, and the virtual key codes its keys and the
// headless --key steps use.
#pragma once

#include "app/app_state.hpp"

namespace os2app {

// Virtual key codes (layout independent).
enum Key : unsigned short {
    kA = 0, kS = 1, kD = 2, kF = 3, kZ = 6, kX = 7, kQ = 12, kW = 13, kE = 14, kR = 15,
    kEquals = 24, kMinus = 27, kComma = 43, kPeriod = 47, kPadPlus = 69, kPadMinus = 78,
    kEscape = 53, kSpace = 49, kHome = 115, kEnd = 119, kPageUp = 116, kPageDown = 121, kLeft = 123, kRight = 124, kDown = 125, kUp = 126,
    kF1 = 122, kF2 = 120, kF3 = 99, kF4 = 118, kF5 = 96, kReturn = 36, kEnter = 76, kP = 35, k1 = 18, k2 = 19, k3 = 20, kY = 16, kBackspace = 51, kForwardDelete = 117,
};

} // namespace os2app

// MTKView that keeps the camera and the keys held down.
@interface OS2View : MTKView
- (os2::LotCamera*)camera;
- (void)setCamera:(std::unique_ptr<os2::LotCamera>)camera;
- (void)applyHeldKeys:(double)dt;
- (void)holdKey:(unsigned short)key seconds:(double)seconds;
- (void)refreshCursor:(NSEvent*)e;
@end
