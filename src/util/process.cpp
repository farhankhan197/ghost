#include "process.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#endif

namespace ghost {
namespace util {

namespace {

static void trimTrailingNewlines(std::string& value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
}

#ifdef _WIN32

static std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return L"";
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size);
    return out;
}

static std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), size, nullptr, nullptr);
    return out;
}

static std::wstring quoteWindowsArg(const std::string& arg) {
    bool needsQuotes = arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string::npos;
    std::wstring warg = utf8ToWide(arg);
    if (!needsQuotes) return warg;

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : warg) {
        if (c == L'\\') {
            backslashes++;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(c);
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

static std::wstring commandLine(const Process::Command& command) {
    std::wstring line = quoteWindowsArg(command.executable);
    for (const auto& arg : command.args) {
        line.push_back(L' ');
        line += quoteWindowsArg(arg);
    }
    return line;
}

static void readHandle(HANDLE handle, std::string& output) {
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(handle, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, buffer + read);
    }
}

static void writeHandle(HANDLE handle, const std::string& input) {
    if (!input.empty()) {
        DWORD written = 0;
        WriteFile(handle, input.data(), static_cast<DWORD>(input.size()), &written, nullptr);
    }
}

#else

static void readFd(int fd, std::string& output) {
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        output.append(buffer, buffer + n);
    }
}

static void writeFd(int fd, const std::string& input) {
    const char* data = input.data();
    size_t remaining = input.size();
    while (remaining > 0) {
        ssize_t written = write(fd, data, remaining);
        if (written <= 0) break;
        data += written;
        remaining -= static_cast<size_t>(written);
    }
}

#endif

}

Process::Result Process::capture(const Command& command) {
    Result result;
    if (command.executable.empty()) return result;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;

    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) return result;
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    if (!command.mergeStderr) {
        if (!CreatePipe(&stderrRead, &stderrWrite, &sa, 0)) {
            CloseHandle(stdoutRead);
            CloseHandle(stdoutWrite);
            return result;
        }
        SetHandleInformation(stderrRead, HANDLE_FLAG_INHERIT, 0);
    }

    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0)) {
        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        if (stderrRead) CloseHandle(stderrRead);
        if (stderrWrite) CloseHandle(stderrWrite);
        return result;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(STARTUPINFOW);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = command.mergeStderr ? stdoutWrite : stderrWrite;

    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = commandLine(command);
    std::wstring cwd = utf8ToWide(command.cwd);

    BOOL ok = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si,
        &pi
    );

    CloseHandle(stdinRead);
    CloseHandle(stdoutWrite);
    if (stderrWrite) CloseHandle(stderrWrite);

    if (!ok) {
        CloseHandle(stdinWrite);
        CloseHandle(stdoutRead);
        if (stderrRead) CloseHandle(stderrRead);
        return result;
    }

    std::thread stdinThread(writeHandle, stdinWrite, std::cref(command.stdinText));
    std::thread stdoutThread(readHandle, stdoutRead, std::ref(result.stdoutText));
    std::thread stderrThread;
    if (!command.mergeStderr) {
        stderrThread = std::thread(readHandle, stderrRead, std::ref(result.stderrText));
    }

    if (stdinThread.joinable()) stdinThread.join();
    CloseHandle(stdinWrite);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    result.exitCode = static_cast<int>(exitCode);

    if (stdoutThread.joinable()) stdoutThread.join();
    CloseHandle(stdoutRead);
    if (stderrThread.joinable()) stderrThread.join();
    if (stderrRead) CloseHandle(stderrRead);

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
#else
    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    if (pipe(stdinPipe) != 0 || pipe(stdoutPipe) != 0 || (!command.mergeStderr && pipe(stderrPipe) != 0)) {
        if (stdinPipe[0] != -1) close(stdinPipe[0]);
        if (stdinPipe[1] != -1) close(stdinPipe[1]);
        if (stdoutPipe[0] != -1) close(stdoutPipe[0]);
        if (stdoutPipe[1] != -1) close(stdoutPipe[1]);
        if (stderrPipe[0] != -1) close(stderrPipe[0]);
        if (stderrPipe[1] != -1) close(stderrPipe[1]);
        return result;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(command.mergeStderr ? stdoutPipe[1] : stderrPipe[1], STDERR_FILENO);

        close(stdinPipe[0]);
        close(stdinPipe[1]);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        if (!command.mergeStderr) {
            close(stderrPipe[0]);
            close(stderrPipe[1]);
        }

        if (!command.cwd.empty()) {
            chdir(command.cwd.c_str());
        }

        std::vector<std::string> storage;
        storage.reserve(command.args.size() + 1);
        storage.push_back(command.executable);
        storage.insert(storage.end(), command.args.begin(), command.args.end());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& item : storage) {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);
        execvp(command.executable.c_str(), argv.data());
        _exit(127);
    }

    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    if (!command.mergeStderr) close(stderrPipe[1]);

    std::thread stdinThread(writeFd, stdinPipe[1], std::cref(command.stdinText));
    std::thread stdoutThread(readFd, stdoutPipe[0], std::ref(result.stdoutText));
    std::thread stderrThread;
    if (!command.mergeStderr) {
        stderrThread = std::thread(readFd, stderrPipe[0], std::ref(result.stderrText));
    }

    if (stdinThread.joinable()) stdinThread.join();
    close(stdinPipe[1]);

    int status = 0;
    if (pid > 0) {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) result.exitCode = 128 + WTERMSIG(status);
    }

    if (stdoutThread.joinable()) stdoutThread.join();
    close(stdoutPipe[0]);
    if (stderrThread.joinable()) stderrThread.join();
    if (!command.mergeStderr) close(stderrPipe[0]);
#endif

    trimTrailingNewlines(result.stdoutText);
    trimTrailingNewlines(result.stderrText);
    return result;
}

Process::Result Process::run(const Command& command) {
    Command copy = command;
    copy.mergeStderr = true;
    return capture(copy);
}

std::string Process::capture(const std::string& command) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.c_str(), "r"), pclose);
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    trimTrailingNewlines(result);

    return result;
}

}
}
