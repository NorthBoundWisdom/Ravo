#pragma once
#include <QByteArray>

// Synthetic four-quadrant image encoded by the system HEIC writer on macOS.
// This is test input generation, not a production encoder or pixel oracle.
QByteArray make_heic_fixture(int orientation = 1, bool display_p3 = false);
