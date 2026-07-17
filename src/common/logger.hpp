#pragma once
#include <syslog.h>
#include <string>
#include <sstream>

/// Core logging interface utilizing syslog for systemd journal integration.
class Logger {
public:
    /// Initialize logging with identifier.
    static void init(const char *ident) { openlog(ident, LOG_PID | LOG_NDELAY, LOG_USER); }

    /// Log informational message.
    static void info(const std::string &msg) { syslog(LOG_INFO, "%s", msg.c_str()); }

    /// Log warning message.
    static void warn(const std::string &msg) { syslog(LOG_WARNING, "%s", msg.c_str()); }

    /// Log error message.
    static void error(const std::string &msg) { syslog(LOG_ERR, "%s", msg.c_str()); }
};
