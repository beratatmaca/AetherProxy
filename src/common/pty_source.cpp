#include "common/pty_source.hpp"
#include <pty.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdlib>
#include <fcntl.h>

PTYSource::PTYSource(const std::vector<std::string>& cmd) : masterFd(-1), childPid(-1) {
    struct winsize ws{24, 80, 0, 0};
    if (isatty(STDIN_FILENO)) {
        ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
    }

    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid == 0) {
        if (!cmd.empty()) {
            std::vector<char*> execArgs;
            for (const auto& arg : cmd) {
                execArgs.push_back(const_cast<char*>(arg.c_str()));
            }
            execArgs.push_back(nullptr);
            execvp(execArgs[0], execArgs.data());
        } else {
            const char* shell = std::getenv("SHELL");
            if (!shell) {
                shell = "/bin/sh";
            }
            execl(shell, shell, nullptr);
        }
        std::exit(1);
    } else if (pid > 0) {
        masterFd = master;
        childPid = pid;
        int flags = fcntl(masterFd, F_GETFL, 0);
        if (flags != -1) {
            fcntl(masterFd, F_SETFL, flags | O_NONBLOCK);
        }
    }
}

PTYSource::~PTYSource() {
    if (masterFd != -1) {
        close(masterFd);
    }
    if (childPid > 0) {
        kill(childPid, SIGTERM);
        int status = 0;
        waitpid(childPid, &status, 0);
    }
}

ssize_t PTYSource::read(char* buf, size_t len) {
    if (masterFd == -1) {
        return -1;
    }
    return ::read(masterFd, buf, len);
}

ssize_t PTYSource::write(const char* buf, size_t len) {
    if (masterFd == -1) {
        return -1;
    }
    return ::write(masterFd, buf, len);
}

bool PTYSource::isReadOnly() const {
    return false;
}

winsize PTYSource::getSize() const {
    winsize ws{0, 0, 0, 0};
    if (masterFd != -1) {
        ioctl(masterFd, TIOCGWINSZ, &ws);
    }
    return ws;
}

void PTYSource::setSize(winsize ws) {
    if (masterFd != -1) {
        ioctl(masterFd, TIOCSWINSZ, &ws);
    }
}

int PTYSource::getFd() const {
    return masterFd;
}
