#pragma once
#include <sys/types.h>

/// Decouples WebRTC from IO backends.
class IOSource {
public:
    virtual ~IOSource() = default;

    /// Reads data from source.
    virtual ssize_t read(char *buf, size_t len) = 0;

    /// Writes data to source.
    virtual ssize_t write(const char *buf, size_t len) = 0;

    /// Returns read-only state.
    virtual bool isReadOnly() const = 0;

    /// Gets internal descriptor.
    virtual int getFd() const = 0;
};
