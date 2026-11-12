#pragma once

// Starting helper programs (xdg-open, zenity, notify-send) on POSIX without a shell: arguments are passed as
// argv, so nothing in them is ever interpreted.

#if !defined(_WIN32)

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <thread>
#include <vector>

extern char** environ;

namespace cppdesk::spawn {

inline std::vector<char*> argvOf(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    return argv;
}

// Starts the program found on PATH and reaps it in the background. False if it could not start.
inline bool detached(const std::vector<std::string>& args) {
    if (args.empty()) return false;
    std::vector<char*> argv = argvOf(args);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return false;
    std::thread([pid] { int status = 0; waitpid(pid, &status, 0); }).detach();
    return true;
}

// Runs the program to completion and returns its standard output; nullopt if it could not start or exited
// with a non-zero status (a cancelled dialog).
inline std::optional<std::string> output(const std::vector<std::string>& args) {
    if (args.empty()) return std::nullopt;
    int fds[2];
    if (pipe(fds) != 0) return std::nullopt;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    std::vector<char*> argv = argvOf(args);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return std::nullopt;
    }
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::nullopt;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// True when the program exists on PATH.
inline bool onPath(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string p(path);
    size_t start = 0;
    while (start <= p.size()) {
        const size_t end = p.find(':', start);
        const std::string dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && access((dir + "/" + program).c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

} // namespace cppdesk::spawn

#endif
