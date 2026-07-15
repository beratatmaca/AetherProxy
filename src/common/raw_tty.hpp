#pragma once

/// Puts the controlling terminal into raw mode.
bool enableRawTty();

/// Restores the saved terminal state.
void disableRawTty();
