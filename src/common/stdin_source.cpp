#include "common/stdin_source.hpp"
#include <unistd.h>
#include <fcntl.h>

StdinSource::StdinSource() {
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1) {
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    }
}

ssize_t StdinSource::read(char* buf, size_t len) {
    return ::read(STDIN_FILENO, buf, len);
}

ssize_t StdinSource::write(const char*, size_t) {
    return -1;
}

bool StdinSource::isReadOnly() const {
    return true;
}

int StdinSource::getFd() const {
    return STDIN_FILENO;
}
