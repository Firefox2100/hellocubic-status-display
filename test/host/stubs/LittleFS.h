// Host stand-in for the parts of LittleFS / fs::File that src/events/EventStore.cpp uses, backed by stdio files
// under a directory the test chooses (LittleFSStub::root).
#pragma once
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <string>

class File {
   public:
    File() = default;
    explicit File(FILE* file) : file_(file) {}
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : file_(other.file_) { other.file_ = nullptr; }
    File& operator=(File&& other) noexcept {
        close();
        file_ = other.file_;
        other.file_ = nullptr;
        return *this;
    }
    ~File() { close(); }
    explicit operator bool() const { return file_ != nullptr; }
    size_t size() {
        if (file_ == nullptr) return 0;
        fflush(file_);
        struct stat info;
        return fstat(fileno(file_), &info) == 0 ? static_cast<size_t>(info.st_size) : 0;
    }
    bool seek(size_t position) { return file_ != nullptr && fseek(file_, static_cast<long>(position), SEEK_SET) == 0; }
    size_t read(uint8_t* buffer, size_t length) { return file_ ? fread(buffer, 1, length, file_) : 0; }
    size_t write(const uint8_t* buffer, size_t length) { return file_ ? fwrite(buffer, 1, length, file_) : 0; }
    void close() {
        if (file_ != nullptr) fclose(file_);
        file_ = nullptr;
    }

   private:
    FILE* file_ = nullptr;
};

struct LittleFSStub {
    static inline std::string root;
    static std::string path(const char* name) { return root + name; }
    File open(const char* name, const char* mode) {
        const std::string m = std::string(mode) == "r" ? "rb" : std::string(mode) == "w" ? "wb" : std::string(mode) == "a" ? "ab" : "r+b";
        return File(fopen(path(name).c_str(), m.c_str()));
    }
    bool remove(const char* name) { return ::remove(path(name).c_str()) == 0; }
    bool rename(const char* from, const char* to) { return ::rename(path(from).c_str(), path(to).c_str()) == 0; }
    bool exists(const char* name) { return access(path(name).c_str(), F_OK) == 0; }
};
inline LittleFSStub LittleFS;
