#include "WindowsIO.hpp"
#include "Protocol.hpp"

#include <algorithm>
#include <climits>
#include <limits>
#include <sstream>
#include <vector>

namespace skb {
namespace win {

static Error failure(const std::string& operation) {
    return Error("io_error", operation + ": Win32 error " + std::to_string(GetLastError()));
}

std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    if (text.size() > INT_MAX) {
        throw Error("bad_unicode", "String too long");
    }

    const int wideLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0);
    if (!wideLength) {
        throw failure("Invalid UTF-8");
    }

    std::wstring out(wideLength, L'\0');
    if (!MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            &out[0],
            wideLength)) {
        throw failure("UTF-8 conversion");
    }
    return out;
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int utf8Length = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (!utf8Length) {
        throw failure("Invalid UTF-16");
    }

    std::string out(utf8Length, '\0');
    if (!WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            static_cast<int>(text.size()),
            &out[0],
            utf8Length,
            nullptr,
            nullptr)) {
        throw failure("UTF-16 conversion");
    }
    return out;
}

std::wstring environment(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (!required) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    const DWORD written = GetEnvironmentVariableW(name, buffer.data(), required);
    if (!written || written >= required) {
        throw failure("Read environment variable");
    }
    return std::wstring(buffer.data(), written);
}

std::wstring canonical(const std::wstring& path) {
    const bool driveAbsolute =
        path.size() >= 3
        && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))
        && path[1] == L':'
        && (path[2] == L'\\' || path[2] == L'/');
    if (!driveAbsolute) {
        throw Error("bad_path", "Use an absolute local drive path");
    }

    const bool hasForbiddenCharacter =
        path.find_first_of(L"\r\n") != std::wstring::npos
        || path.find(L'\0') != std::wstring::npos
        || path.find(L':', 2) != std::wstring::npos;
    if (hasForbiddenCharacter) {
        throw Error("bad_path", "Invalid path or alternate data stream");
    }

    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    if (!length || length >= MAX_PATH) {
        throw Error("bad_path", "Path exceeds legacy MAX_PATH");
    }
    return std::wstring(buffer, length);
}

bool samePath(const std::wstring& left, const std::wstring& right) {
    const auto canonicalLeft = canonical(left);
    const auto canonicalRight = canonical(right);
    return CompareStringOrdinal(
               canonicalLeft.c_str(),
               -1,
               canonicalRight.c_str(),
               -1,
               TRUE)
        == CSTR_EQUAL;
}

bool exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return false;
    }
    throw failure("GetFileAttributes");
}

std::wstring join(const std::wstring& directory, const std::wstring& name) {
    return directory + L"\\" + name;
}

std::string read(const std::wstring& path, size_t maximum) {
    Handle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    if (!file.valid()) {
        throw failure("Open input");
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.get(), &size)) {
        throw failure("Input size");
    }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maximum) {
        throw Error("file_too_large", "Input exceeds size limit");
    }

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD readCount = 0;
    if (!data.empty()
        && (!ReadFile(file.get(), &data[0], static_cast<DWORD>(data.size()), &readCount, nullptr)
            || readCount != data.size())) {
        throw failure("Read input");
    }
    return data;
}

void moveNew(const std::wstring& from, const std::wstring& to) {
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        throw failure("Atomic rename (destination must be new)");
    }
}

void atomicWrite(const std::wstring& path, const std::string& text) {
    if (text.size() > MAXDWORD) {
        throw Error("file_too_large", "Output exceeds DWORD");
    }

    const auto temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
    bool created = false;
    try {
        {
            Handle file(CreateFileW(
                temporary.c_str(),
                GENERIC_WRITE,
                0,
                nullptr,
                CREATE_NEW,
                FILE_ATTRIBUTE_NORMAL,
                nullptr));
            if (!file.valid()) {
                throw failure("Create result temp file");
            }
            created = true;

            DWORD written = 0;
            if (!WriteFile(
                    file.get(),
                    text.data(),
                    static_cast<DWORD>(text.size()),
                    &written,
                    nullptr)
                || written != text.size()) {
                throw failure("Write result");
            }
            if (!FlushFileBuffers(file.get())) {
                throw failure("Flush result");
            }
        }
        moveNew(temporary, path);
    } catch (...) {
        if (created) {
            DeleteFileW(temporary.c_str());
        }
        throw;
    }
}

std::vector<std::wstring> list(const std::wstring& directory, const wchar_t* pattern) {
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW(join(directory, pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) {
            return {};
        }
        throw failure("List queue");
    }

    std::vector<std::wstring> names;
    try {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                names.emplace_back(found.cFileName);
            }
        } while (FindNextFileW(search, &found));

        if (GetLastError() != ERROR_NO_MORE_FILES) {
            throw failure("Enumerate queue");
        }
    } catch (...) {
        FindClose(search);
        throw;
    }

    FindClose(search);
    std::sort(names.begin(), names.end());
    return names;
}

void log(const std::wstring& directory, const std::string& message) noexcept {
    try {
        const std::string line = std::to_string(GetTickCount64()) + " " + message + "\r\n";
        OutputDebugStringA(("SoundKeysBridge " + line).c_str());
        if (directory.empty()) {
            return;
        }

        Handle file(CreateFileW(
            join(directory, L"bridge.log").c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!file.valid()) {
            return;
        }

        DWORD written = 0;
        WriteFile(file.get(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    } catch (...) {
    }
}

std::wstring executable() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) {
        throw failure("Executable path");
    }
    return std::wstring(path, length);
}

std::string fileVersion(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!size) {
        return "unavailable";
    }

    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return "unavailable";
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize)
        || infoSize < sizeof(*info)) {
        return "unavailable";
    }

    return std::to_string(HIWORD(info->dwFileVersionMS)) + "."
        + std::to_string(LOWORD(info->dwFileVersionMS)) + "."
        + std::to_string(HIWORD(info->dwFileVersionLS)) + "."
        + std::to_string(LOWORD(info->dwFileVersionLS));
}

void requireNewAep(const std::wstring& path) {
    const auto canonicalPath = canonical(path);
    if (canonicalPath.size() < 4
        || _wcsicmp(canonicalPath.c_str() + canonicalPath.size() - 4, L".aep") != 0) {
        throw Error("bad_path", "Output must end in .aep");
    }
    if (exists(canonicalPath)) {
        throw Error("output_exists", "Refusing to overwrite output project");
    }

    auto parent = canonicalPath.substr(0, canonicalPath.find_last_of(L"\\"));
    if (parent.size() == 2) {
        parent += L"\\";
    }

    const DWORD attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        throw Error("bad_path", "Output parent directory does not exist");
    }
}

void requireNonemptyFile(const std::wstring& path) {
    Handle file(CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));
    LARGE_INTEGER size = {};
    if (!file.valid() || !GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0) {
        throw Error("save_failed", "Saved project is missing or empty");
    }
}

}  // namespace win
}  // namespace skb
