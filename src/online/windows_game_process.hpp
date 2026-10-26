#pragma once
// Windows process details for the disposable match owner.
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>
#include <windows.h>

namespace online::detail {
[[noreturn]] inline void windows_fail(const char* operation) {
    throw std::system_error(GetLastError(), std::system_category(), operation);
}
using WindowsHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

inline std::wstring wide_utf8(const std::string& text) {
    if (text.find('\0') != std::string::npos) throw std::invalid_argument("embedded null in process argument");
    if (text.empty()) return {};
    if (text.size() > INT_MAX) throw std::invalid_argument("process argument is too long");
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!size) windows_fail("decode process argument");
    std::wstring result(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), size))
        windows_fail("decode process argument");
    return result;
}

inline std::wstring quote_argument(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += ch; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}

inline std::filesystem::path make_match_directory() {
    auto root = std::filesystem::temp_directory_path();
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        auto path = root / (L"bm64-match-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(std::random_device{}()));
        if (std::filesystem::create_directory(path)) return path;
    }
    throw std::runtime_error("cannot create isolated match settings");
}

struct WindowsGameProcess { HANDLE process, job; };
inline WindowsGameProcess launch_windows_game(const std::filesystem::path& executable, const std::filesystem::path& log,
                                 const std::filesystem::path& settings, HANDLE channel,
                                 const std::vector<std::string>& arguments,
                                 const std::map<std::string, std::string>& environment) {
    auto executable_path = std::filesystem::absolute(executable).wstring();
    auto command = quote_argument(executable_path);
    for (const auto& argument : arguments) command += L' ' + quote_argument(wide_utf8(argument));

    auto* inherited = GetEnvironmentStringsW();
    if (!inherited) windows_fail("read process environment");
    std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited_owner(inherited, FreeEnvironmentStringsW);
    std::vector<std::wstring> entries;
    for (auto* entry = inherited; *entry; entry += std::wcslen(entry) + 1) {
        if (_wcsnicmp(entry, L"BM64_", 5) && _wcsnicmp(entry, L"APP_FOLDER_PATH=", 16))
            entries.emplace_back(entry);
    }
    for (const auto& [key, value] : environment)
        if (key != "BM64_OWNER_CHANNEL") entries.push_back(wide_utf8(key + "=" + value));
    entries.push_back(L"APP_FOLDER_PATH=" + settings.wstring());
    entries.push_back(L"BM64_OWNER_CHANNEL=" + std::to_wstring(reinterpret_cast<uintptr_t>(channel)));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::vector<wchar_t> block;
    for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0); }
    block.push_back(0);

    SECURITY_ATTRIBUTES access{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    WindowsHandle output(CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &access, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr), CloseHandle);
    if (output.get() == INVALID_HANDLE_VALUE) windows_fail("open match log");
    WindowsHandle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &access,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr), CloseHandle);
    if (input.get() == INVALID_HANDLE_VALUE) windows_fail("open match input");
    if (!SetHandleInformation(channel, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) windows_fail("inherit match pipe");

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (!size) windows_fail("size process attributes");
    std::vector<uint8_t> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) windows_fail("initialize process attributes");
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&DeleteProcThreadAttributeList)>
        attributes_owner(attributes, DeleteProcThreadAttributeList);
    HANDLE handles[]{channel, input.get(), output.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, sizeof(handles), nullptr, nullptr))
        windows_fail("isolate inherited match handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.get();
    startup.lpAttributeList = attributes;
    WindowsHandle job(CreateJobObjectW(nullptr, nullptr), CloseHandle);
    if (!job) windows_fail("create match process job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        windows_fail("configure match process cleanup");
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable_path.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        block.data(), nullptr, &startup.StartupInfo, &process)) windows_fail("launch game");
    WindowsHandle process_owner(process.hProcess, CloseHandle), thread_owner(process.hThread, CloseHandle);
    // Attach before any game code can run, including a child that immediately exits.
    if (!AssignProcessToJobObject(job.get(), process.hProcess) || ResumeThread(process.hThread) == DWORD(-1)) {
        auto error = GetLastError();
        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE);
        SetLastError(error); windows_fail("own match process");
    }
    return {process_owner.release(), job.release()};
}
}
