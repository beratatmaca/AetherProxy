#pragma once
#include "common/io_source.hpp"
#include <sys/ioctl.h>
#include <termios.h>
#include <vector>
#include <string>

/// IO source wrapping a PTY.
class PTYSource : public IOSource {
public:
    /// Creates a PTY source with custom command.
    PTYSource(const std::vector<std::string> &cmd = {});

    /// Cleans up the descriptors.
    ~PTYSource() override;

    /// Reads data from PTY.
    ssize_t read(char *buf, size_t len) override;

    /// Writes data to PTY.
    ssize_t write(const char *buf, size_t len) override;

    /// Returns false.
    bool isReadOnly() const override;

    /// Gets terminal window size.
    winsize getSize() const;

    /// Sets terminal window size.
    void setSize(winsize ws) const;

    /// Gets master fd.
    int getFd() const override;

private:
    int masterFd = -1;
    int childPid = -1;
};
