#include "common/raw_tty.hpp"
#include <termios.h>
#include <unistd.h>
#include <cstdlib>

static struct termios orig_termios;
static bool raw_active = false;

void disableRawTty() {
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_active = false;
    }
}

bool enableRawTty() {
    if (isatty(STDIN_FILENO) == 0) {
        return false;
    }
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        return false;
    }
    struct termios raw = orig_termios;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST | ONLCR;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        return false;
    }
    if (!raw_active) {
        std::atexit(disableRawTty);
    }
    raw_active = true;
    return true;
}
