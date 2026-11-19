#include "command_palette.hpp"
#include <algorithm>
#include <cctype>
#include <thread>
#include <chrono>

#if !defined(_WIN32)
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#include <cerrno>
#endif

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

CommandPaletteManager::~CommandPaletteManager() {
    shutdown();
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
                                        uint32_t timeoutMs,
                                        const std::atomic<bool>* cancelFlag) {
    outOutput.clear();
    outExitCode = -1;

    int outPipe[2];
    if (pipe(outPipe) != 0) {
        outOutput = "Error: Failed to create stdout pipe";
        return false;
    }
    fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(outPipe[1], F_SETFD, FD_CLOEXEC);

    struct rlimit rl{};
    const int maxFd = (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY)
                          ? static_cast<int>(std::min<rlim_t>(rl.rlim_cur, 65536)) : 65536;

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int devNull = open("/dev/null", O_RDONLY);
        if (devNull >= 0) {
            dup2(devNull, STDIN_FILENO);
            if (devNull > 2) close(devNull);
        } else {
            close(STDIN_FILENO);
        }
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);
        for (int fd = 3; fd < maxFd; ++fd) close(fd);
        execl("/bin/sh", "sh", "-c", cmdLine.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(outPipe[1]);
    if (pid < 0) {
        close(outPipe[0]);
        outOutput = "Error: Failed to fork process";
        return false;
    }

    int fl = fcntl(outPipe[0], F_GETFL, 0);
    if (fl >= 0) fcntl(outPipe[0], F_SETFL, fl | O_NONBLOCK);

    auto startTime = std::chrono::steady_clock::now();
    constexpr size_t maxOutputBytes = 65536;
    bool timedOut = false;
    bool cancelled = false;
    bool childExited = false;
    int status = 0;

    while (true) {
        if (cancelFlag && cancelFlag->load()) {
            cancelled = true;
            break;
        }

        pollfd pfd{ outPipe[0], POLLIN, 0 };
        int pr = poll(&pfd, 1, 20);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            char buffer[4096];
            bool pipeEof = false;
            while (true) {
                ssize_t n = read(outPipe[0], buffer, sizeof(buffer));
                if (n > 0) {
                    if (outOutput.size() < maxOutputBytes) {
                        size_t toAppend = std::min<size_t>(static_cast<size_t>(n), maxOutputBytes - outOutput.size());
                        outOutput.append(buffer, toAppend);
                    }
                } else if (n == 0) {
                    pipeEof = true;
                    break;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    pipeEof = true;
                    break;
                }
            }
            if (pipeEof) {
                break;
            }
        }

        pid_t wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            childExited = true;
            // Drain any remaining buffered pipe output
            char buffer[4096];
            while (true) {
                ssize_t n = read(outPipe[0], buffer, sizeof(buffer));
                if (n > 0) {
                    if (outOutput.size() < maxOutputBytes) {
                        size_t toAppend = std::min<size_t>(static_cast<size_t>(n), maxOutputBytes - outOutput.size());
                        outOutput.append(buffer, toAppend);
                    }
                } else if (n < 0 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime).count();
        if (elapsed > static_cast<int64_t>(timeoutMs)) {
            timedOut = true;
            break;
        }
    }

    close(outPipe[0]);

    if (timedOut || cancelled) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        if (timedOut) {
            outOutput += "\n[Execution timed out after " + std::to_string(timeoutMs) + " ms]";
        }
        outExitCode = -1;
        return !cancelled;
    }

    if (!childExited) {
        // Wait briefly for child after EOF on stdout, then kill if still lingering
        for (int i = 0; i < 25; ++i) {
            pid_t wr = waitpid(pid, &status, WNOHANG);
            if (wr == pid) {
                childExited = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!childExited) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }

    if (WIFEXITED(status)) {
        outExitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        outExitCode = 128 + WTERMSIG(status);
    } else {
        outExitCode = -1;
    }
    return true;
}
#else
bool CommandPaletteManager::executeSync(const std::string& cmdLine,
                                        int32_t& outExitCode,
                                        std::string& outOutput,
                                        uint32_t timeoutMs,
                                        const std::atomic<bool>* cancelFlag) {
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
    // Restrict inherited handles strictly to the child's stdio pipes so live sockets are not leaked to child processes
    HANDLE inheritOnly[2] = { hStdinRead, hWritePipe };
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    const bool attrsInited = attrSize > 0 && InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
    STARTUPINFOEXW six{};
    six.StartupInfo = si;
    six.StartupInfo.cb = sizeof(six);
    if (attrsInited && UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritOnly,
                                                 sizeof(inheritOnly), nullptr, nullptr)) {
        six.lpAttributeList = attrs;
    }

    BOOL created = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &six.StartupInfo,
        &pi
    );
    if (attrsInited) DeleteProcThreadAttributeList(attrs);

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
        if (cancelFlag && cancelFlag->load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 500);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            CloseHandle(hReadPipe);
            outExitCode = -1;
            return false;
        }

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

void CommandPaletteManager::workerLoop() {
    while (true) {
        PendingTask task;
        {
            std::unique_lock<std::mutex> lk(poolMutex_);
            poolCv_.wait(lk, [&]() {
                return shuttingDown_ || !taskQueue_.empty();
            });
            if (shuttingDown_) break;
            task = std::move(taskQueue_.front());
            taskQueue_.pop_front();
        }

        int32_t exitCode = -1;
        std::string output;
        executeSync(task.cmdLine, exitCode, output, 15000, &cancelFlag_);
        if (!cancelFlag_.load() && task.onComplete) {
            task.onComplete(exitCode, output);
        }
    }
}

void CommandPaletteManager::executeAsync(const std::string& cmdLine,
                                         std::function<void(int32_t exitCode, const std::string& output)> onComplete) {
    constexpr size_t kMaxWorkers = 2;
    constexpr size_t kMaxPendingTasks = 16;

    std::unique_lock<std::mutex> lk(poolMutex_);
    if (shuttingDown_) {
        return;
    }
    if (taskQueue_.size() >= kMaxPendingTasks) {
        lk.unlock();
        if (onComplete) {
            onComplete(-1, "Error: Command execution queue is full");
        }
        return;
    }
    if (workers_.size() < kMaxWorkers) {
        workers_.emplace_back(&CommandPaletteManager::workerLoop, this);
    }
    taskQueue_.push_back({ cmdLine, std::move(onComplete) });
    lk.unlock();
    poolCv_.notify_one();
}

void CommandPaletteManager::shutdown() {
    std::vector<std::thread> toJoin;
    {
        std::lock_guard<std::mutex> lk(poolMutex_);
        shuttingDown_ = true;
        cancelFlag_.store(true);
        taskQueue_.clear();
        toJoin.swap(workers_);
    }
    poolCv_.notify_all();
    for (auto& t : toJoin) {
        if (t.joinable()) {
            try { t.join(); } catch (...) {}
        }
    }
}

} // namespace cppdesk
