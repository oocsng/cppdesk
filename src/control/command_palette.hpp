#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <vector>
#include <functional>
#include <mutex>
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
    ~CommandPaletteManager() = default;

    // Catalog queries
    const std::vector<OmnibarCommand>& getCatalog() const;
    std::vector<OmnibarCommand> search(const std::string& query) const;
    void registerCommand(const OmnibarCommand& cmd);

    // Asynchronous non-blocking command execution via pipes
    void executeAsync(const std::string& cmdLine,
                      std::function<void(int32_t exitCode, const std::string& output)> onComplete);

    // Synchronous execution helper for testing and standalone calls
    static bool executeSync(const std::string& cmdLine,
                            int32_t& outExitCode,
                            std::string& outOutput,
                            uint32_t timeoutMs = 5000);

private:
    void initDefaultCatalog();

    mutable std::mutex          mutex_;
    std::vector<OmnibarCommand> catalog_;
};

} // namespace cppdesk
