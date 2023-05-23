#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace skb {
namespace win {

class Handle {
public:
    explicit Handle(HANDLE handle = INVALID_HANDLE_VALUE)
        : handle_(handle) {}

    ~Handle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    bool valid() const {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

    HANDLE get() const { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::wstring wide(const std::string& text);
std::string utf8(const std::wstring& text);
std::wstring environment(const wchar_t* name);
std::wstring canonical(const std::wstring& path);
bool samePath(const std::wstring& left, const std::wstring& right);
bool exists(const std::wstring& path);
std::wstring join(const std::wstring& directory, const std::wstring& name);
std::string read(const std::wstring& path, size_t maximum = 65536);
void atomicWrite(const std::wstring& path, const std::string& text);  // never overwrites
void moveNew(const std::wstring& from, const std::wstring& to);       // never overwrites
std::vector<std::wstring> list(const std::wstring& directory, const wchar_t* pattern);
void log(const std::wstring& directory, const std::string& message) noexcept;
std::wstring executable();
std::string fileVersion(const std::wstring& path);
void requireNewAep(const std::wstring& path);
void requireNonemptyFile(const std::wstring& path);

}  // namespace win
}  // namespace skb
