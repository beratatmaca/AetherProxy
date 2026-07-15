#pragma once
#include "common/io_source.hpp"

/// IO source wrapping stdin.
class StdinSource : public IOSource {
public:
    /// Creates a stdin source.
    StdinSource();

    /// Reads data from stdin.
    ssize_t read(char* buf, size_t len) override;

    /// Writes returns -1.
    ssize_t write(const char* buf, size_t len) override;

    /// Returns true.
    bool isReadOnly() const override;

    /// Gets internal descriptor.
    int getFd() const override;
};
