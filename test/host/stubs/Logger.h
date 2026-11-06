// Host stand-in for lib/Logger: logging is discarded.
#pragma once
struct Logger {
    static void info(const char*, const char* = nullptr) {}
    static void warn(const char*, const char* = nullptr) {}
    static void error(const char*, const char* = nullptr) {}
};
