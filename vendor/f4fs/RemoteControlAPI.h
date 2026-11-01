#pragma once

// F4FS remote-control provider ABI v1. Project-owned GPL-3.0 code.
// Fixed-width fields, plain callbacks and caller-owned result buffers only.
// No engine, JSON-library, STL object or allocation ownership crosses this ABI.
#include <cstdint>

namespace f4fs::remote::api
{
    inline constexpr std::uint32_t kVersion = 1;
    inline constexpr std::uint32_t kMaxArgumentsBytes = 65536;
    inline constexpr std::uint32_t kMaxResultBytes = 262144;
    inline constexpr const char* kProviderModule = "F4FS.dll";
    inline constexpr const char* kProviderExport = "F4FS_RemoteControl_GetAPI";

    enum class Result : std::uint32_t
    {
        Written = 0,
        InvalidArgument = 1,
        Unavailable = 2,
        InternalFailure = 3,
        WriterFailed = 4
    };

    // Called synchronously at most once. The writer must copy the bytes before
    // returning; neither side retains pointers belonging to the other DLL.
    using WriteFn = bool (*)(void* sink, const char* utf8, std::uint32_t bytes) noexcept;
    struct ProviderV1
    {
        std::uint32_t version;
        std::uint32_t size;
        // Describe writes a JSON array of MCP Tool descriptors. Invoke writes
        // {"ok":true,"result":...} or {"ok":false,"error":{"code":...,"message":...}}.
        Result (*describe)(WriteFn writer, void* sink) noexcept;
        Result (*invoke)(const char* tool, std::uint32_t toolBytes,
            const char* arguments, std::uint32_t argumentBytes,
            WriteFn writer, void* sink) noexcept;
    };
    using GetProviderFn = const ProviderV1* (*)(std::uint32_t version, std::uint32_t callerSize) noexcept;
}

// Provider callbacks remain valid for the process lifetime. Hot-unloading either
// DLL is unsupported. Resolve from an already-loaded module; do not LoadLibrary
// paths supplied by a remote client. A missing/incompatible provider is inert.
