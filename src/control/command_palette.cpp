#include "command_palette.hpp"
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

namespace cppdesk {

static bool containsIgnoreCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) {
            return std::tolower(static_cast<unsigned char>(ch1)) ==
                   std::tolower(static_cast<unsigned char>(ch2));
        }
    );
    return it != haystack.end();
}

CommandPaletteManager::CommandPaletteManager() {
    initDefaultCatalog();
}

void CommandPaletteManager::initDefaultCatalog() {
    std::lock_guard<std::mutex> lock(mutex_);
#if defined(_WIN32)
    catalog_ = {
        {
            "net.ipconfig",
            "Display IP Network Configuration",
            "Network",
            "cmd.exe /c ipconfig /all",
            false
        },
        {
            "net.flushdns",
            "Flush DNS Resolver Cache",
            "Network",
            "cmd.exe /c ipconfig /flushdns",
            false
        },
        {
            "net.ping",
            "Ping Public DNS (8.8.8.8)",
            "Network",
            "cmd.exe /c ping -n 4 8.8.8.8",
            false
        },
        {
            "net.stat",
            "Active Network Connections",
            "Network",
            "cmd.exe /c netstat -ano",
            false
        },
        {
            "net.route",
            "Display IP Routing Table",
            "Network",
            "cmd.exe /c route print",
            false
        },
        {
            "sys.tasklist",
            "List Running Tasks and Processes",
            "System",
            "cmd.exe /c tasklist",
            false
        },
        {
            "sys.info",
            "Display Host System Information",
            "System",
            "cmd.exe /c systeminfo",
            false
        },
        {
            "sys.whoami",
            "Current User and Security Privileges",
            "System",
            "cmd.exe /c whoami /all",
            false
        },
        {
            "diag.services",
            "Query Windows Services State",
            "Diagnostics",
            "cmd.exe /c sc query state= all",
            false
        },
        {
            "diag.disk",
            "Query Disk Free Space and Partitions",
            "Diagnostics",
            "cmd.exe /c wmic logicaldisk get caption,size,freespace",
            false
        },
        {
            "act.reboot60",
            "Schedule Host Restart in 60s",
            "Actions",
            "cmd.exe /c shutdown /r /t 60",
            true
        },
        {
            "act.abort_reboot",
            "Abort Pending Host Restart",
            "Actions",
            "cmd.exe /c shutdown /a",
            true
        }
    };
#elif defined(__APPLE__)
    catalog_ = {
        { "net.ipconfig",     "Display IP Network Configuration",    "Network",     "ifconfig",                false },
        { "net.flushdns",     "Flush DNS Resolver Cache",            "Network",     "dscacheutil -flushcache", false },
        { "net.ping",         "Ping Public DNS (8.8.8.8)",           "Network",     "ping -c 4 8.8.8.8",       false },
        { "net.stat",         "Active Network Connections",          "Network",     "netstat -an",             false },
        { "net.route",        "Display IP Routing Table",            "Network",     "netstat -rn",             false },
        { "sys.tasklist",     "List Running Tasks and Processes",    "System",      "ps aux",                  false },
        { "sys.info",         "Display Host System Information",     "System",      "uname -a",                false },
        { "sys.whoami",       "Current User and Security Privileges","System",      "id",                      false },
        { "diag.services",    "Query System Services State",         "Diagnostics", "launchctl list",          false },
        { "diag.disk",        "Query Disk Free Space and Partitions","Diagnostics", "df -h",                   false },
        { "act.reboot60",     "Schedule Host Restart in 60s",        "Actions",     "shutdown -r +1",          true  },
        { "act.abort_reboot", "Abort Pending Host Restart",          "Actions",     "killall shutdown",        true  }
    };
#else
    catalog_ = {
        { "net.ipconfig",     "Display IP Network Configuration",    "Network",     "ip addr",                                        false },
        { "net.flushdns",     "Flush DNS Resolver Cache",            "Network",     "resolvectl flush-caches",                        false },
        { "net.ping",         "Ping Public DNS (8.8.8.8)",           "Network",     "ping -c 4 8.8.8.8",                              false },
        { "net.stat",         "Active Network Connections",          "Network",     "ss -tunap",                                      false },
        { "net.route",        "Display IP Routing Table",            "Network",     "ip route",                                       false },
        { "sys.tasklist",     "List Running Tasks and Processes",    "System",      "ps aux",                                         false },
        { "sys.info",         "Display Host System Information",     "System",      "uname -a",                                       false },
        { "sys.whoami",       "Current User and Security Privileges","System",      "id",                                             false },
        { "diag.services",    "Query System Services State",         "Diagnostics", "systemctl list-units --type=service --no-pager", false },
        { "diag.disk",        "Query Disk Free Space and Partitions","Diagnostics", "df -h",                                          false },
        { "act.reboot60",     "Schedule Host Restart in 60s",        "Actions",     "shutdown -r +1",                                 true  },
        { "act.abort_reboot", "Abort Pending Host Restart",          "Actions",     "shutdown -c",                                    true  }
    };
#endif
}

std::vector<OmnibarCommand> CommandPaletteManager::getCatalog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return catalog_;
}

std::vector<OmnibarCommand> CommandPaletteManager::search(const std::string& query) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (query.empty()) {
        return catalog_;
    }

    std::vector<OmnibarCommand> results;
    results.reserve(catalog_.size());
    for (const auto& item : catalog_) {
        if (containsIgnoreCase(item.title, query) ||
            containsIgnoreCase(item.category, query) ||
            containsIgnoreCase(item.id, query) ||
            containsIgnoreCase(item.commandLine, query)) {
            results.push_back(item);
        }
    }
    return results;
}

void CommandPaletteManager::registerCommand(const OmnibarCommand& cmd) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : catalog_) {
        if (item.id == cmd.id) {
            item = cmd;
            return;
        }
    }
    catalog_.push_back(cmd);
}

#if !defined(_WIN32)
bool CommandPaletteManager::executeSync(const std::string& cmdLine,
                                        int32_t& outExitCode,
                                        std::string& outOutput,
                                        uint32_t /*timeoutMs*/) {
    outOutput.clear();
    outExitCode = -1;
    FILE* fp = popen(cmdLine.c_str(), "r");
    if (!fp) return false;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp)) {
        outOutput += buf;
    }
    int status = pclose(fp);
    outExitCode = status;
    return true;
}
#else
bool CommandPaletteManager::executeSync(const std::string& cmdLine,
                                        int32_t& outExitCode,
                                        std::string& outOutput,
                                        uint32_t timeoutMs) {
    outOutput.clear();
    outExitCode = -1;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe = NULL;
    HANDLE hWritePipe = NULL;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        outOutput = "Error: Failed to create stdout pipe";
        return false;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // Create stdin pipe so child receives a valid inheritable standard input handle
    HANDLE hStdinRead = NULL;
    HANDLE hStdinWrite = NULL;
    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        outOutput = "Error: Failed to create stdin pipe";
        return false;
    }
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = hStdinRead;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // Proper MultiByteToWideChar UTF-8 conversion
    std::wstring cmdW;
    int len = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, nullptr, 0);
    if (len > 0) {
        cmdW.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, &cmdW[0], len);
        if (!cmdW.empty() && cmdW.back() == L'\0') {
            cmdW.pop_back();
        }
    } else {
        cmdW.assign(cmdLine.begin(), cmdLine.end());
    }

    std::vector<wchar_t> cmdBuf(cmdW.begin(), cmdW.end());
    cmdBuf.push_back(L'\0');

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            CloseHandle(hJob);
            hJob = nullptr;
        }
    }

    BOOL created = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr,
        nullptr,
        &si,
        &pi
    );

    // Immediately close handles child inherited or no longer needed
    CloseHandle(hWritePipe);
    CloseHandle(hStdinRead);
    CloseHandle(hStdinWrite); // Closing write end signals immediate EOF to child process stdin

    if (!created) {
        if (hJob) CloseHandle(hJob);
        CloseHandle(hReadPipe);
        outOutput = "Error: Failed to launch process (Error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    if (hJob && !AssignProcessToJobObject(hJob, pi.hProcess)) {
        CloseHandle(hJob);
        hJob = nullptr;
    }
    ResumeThread(pi.hThread);

    auto startTime = std::chrono::steady_clock::now();
    const size_t maxOutputBytes = 65536;

    while (true) {
        DWORD avail = 0;
        if (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            char buffer[4096];
            DWORD toRead = (std::min)(avail, static_cast<DWORD>(sizeof(buffer)));
            DWORD readNow = 0;
            if (ReadFile(hReadPipe, buffer, toRead, &readNow, NULL) && readNow > 0) {
                if (outOutput.size() < maxOutputBytes) {
                    size_t toAppend = (std::min)(static_cast<size_t>(readNow), maxOutputBytes - outOutput.size());
                    outOutput.append(buffer, toAppend);
                }
            }
        } else {
            DWORD waitRes = WaitForSingleObject(pi.hProcess, 20);
            if (waitRes == WAIT_OBJECT_0) {
                while (PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                    char buffer[4096];
                    DWORD toRead = (std::min)(avail, static_cast<DWORD>(sizeof(buffer)));
                    DWORD readNow = 0;
                    if (ReadFile(hReadPipe, buffer, toRead, &readNow, NULL) && readNow > 0) {
                        if (outOutput.size() < maxOutputBytes) {
                            size_t toAppend = (std::min)(static_cast<size_t>(readNow), maxOutputBytes - outOutput.size());
                            outOutput.append(buffer, toAppend);
                        }
                    } else {
                        break;
                    }
                }
                break;
            }
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed > static_cast<int64_t>(timeoutMs)) {
            if (hJob) {
                TerminateJobObject(hJob, 1);
            } else {
                TerminateProcess(pi.hProcess, 1);
            }
            WaitForSingleObject(pi.hProcess, 500);
            outOutput += "\n[Execution timed out after " + std::to_string(timeoutMs) + " ms]";
            break;
        }
    }

    DWORD rawExitCode = 0;
    GetExitCodeProcess(pi.hProcess, &rawExitCode);
    outExitCode = static_cast<int32_t>(rawExitCode);

    if (hJob) CloseHandle(hJob);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hReadPipe);

    return true;
}
#endif

void CommandPaletteManager::executeAsync(const std::string& cmdLine,
                                         std::function<void(int32_t exitCode, const std::string& output)> onComplete) {
    std::thread([cmdLine, onComplete]() {
        int32_t exitCode = -1;
        std::string output;
        executeSync(cmdLine, exitCode, output, 15000);
        if (onComplete) {
            onComplete(exitCode, output);
        }
    }).detach();
}

} // namespace cppdesk
