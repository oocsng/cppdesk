#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <string>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cstdint>

namespace cppdesk {

struct OmnibarCommand {
    std::string id;
    std::string title;
    std::string category;
    std::string commandLine;
    bool        requiresAdmin = false;
};

class CommandPaletteManager {
public:
    CommandPaletteManager();
    ~CommandPaletteManager();

    CommandPaletteManager(const CommandPaletteManager&) = delete;
    CommandPaletteManager& operator=(const CommandPaletteManager&) = delete;

    // Catalog queries
    const std::vector<OmnibarCommand>& getCatalog() const;
    std::vector<OmnibarCommand> search(const std::string& query) const;
    void registerCommand(const OmnibarCommand& cmd);

    // Asynchronous non-blocking command execution via bounded worker pool
    void executeAsync(const std::string& cmdLine,
                      std::function<void(int32_t exitCode, const std::string& output)> onComplete);

    // Cancels running/queued commands and joins all worker threads
    void shutdown();

    // Synchronous execution helper for testing and standalone calls
    static bool executeSync(const std::string& cmdLine,
                            int32_t& outExitCode,
                            std::string& outOutput,
                            uint32_t timeoutMs = 5000,
                            const std::atomic<bool>* cancelFlag = nullptr);

private:
    struct PendingTask {
        std::string cmdLine;
        std::function<void(int32_t exitCode, const std::string& output)> onComplete;
    };

    void initDefaultCatalog();
    void workerLoop();

    mutable std::mutex          mutex_;
    std::vector<OmnibarCommand> catalog_;

    std::mutex                  poolMutex_;
    std::condition_variable     poolCv_;
    std::deque<PendingTask>     taskQueue_;
    std::vector<std::thread>    workers_;
    bool                        shuttingDown_ = false;
    std::atomic<bool>           cancelFlag_{false};
};

} // namespace cppdesk
