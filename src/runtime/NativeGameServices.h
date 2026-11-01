#pragma once
// Project-owned GPL-3.0 code. CommonLibF4 Game owner adapter; DLL only.
#include "core/ConsoleCapture.h"
#include "core/FrameCapture.h"
#include "core/GameServices.h"
#include "runtime/NativeWorldServices.h"
#include <REL/Version.h>
#include <filesystem>
#include <memory>
#include <mutex>

namespace fusionmcp::runtime {
// Runtimes whose console/log ids are confirmed in the RE symbol database. Every
// engine read and call in this adapter is refused on other runtimes.
bool ConsoleVerifiedRuntime(const REL::Version& runtime) noexcept;

// 0 = 1.10.163, 1 = 1.11.221, 2 = 1.11.240, -1 otherwise.
int VerifiedRuntimeSlot(const REL::Version& runtime) noexcept;

class NativeGameServices final : public GameServices {
public:
    NativeGameServices(bool runtimeVerified,int runtimeSlot);
    bool ConsoleSupported() const noexcept override {return runtimeVerified_;}
    WorldServices* World() noexcept override {return world_.get();}
    // Created on verified runtimes; the present hook marks it installed.
    FrameCapture* Frames() noexcept override {return frames_.get();}
    std::vector<FileEntry> ListLogs() override;
    std::vector<ConsoleRow> ConsoleCommands() override;
    bool ExecuteConsole(const std::string& line) override;
    ConsoleLogSnapshot ReadConsoleLog() override;
    ConsoleRunResult RunConsoleLine(const std::string& line,bool drain=true) override;
    ConsoleCapture* Capture() noexcept override;
    // Plugin load path, once, when the transport is enabled.
    void InstallConsoleHooks();
    ReadinessSample SampleReadiness() override;
    std::vector<FileEntry> ListSaves() override;
    std::vector<FileEntry> ListScreenshots() override;
    bool QueueScreenshot() override;
    // Game owner, after kGameDataReady: caches the engine save directory.
    void ResolveSaveDirectory();
    std::string SaveDirectory() const;
private:
    bool runtimeVerified_;
    int runtimeSlot_;
    bool consoleRuntime_=false;
    ConsoleCapture capture_;
    std::unique_ptr<NativeWorldServices> world_;
    std::unique_ptr<FrameCapture> frames_;
    mutable std::mutex mutex_;
    std::filesystem::path saveDirectory_;
};
}
