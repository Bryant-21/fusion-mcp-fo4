#include "runtime/ConfigFile.h"
#include <F4SE/F4SE.h>
#include <Windows.h>
#include <array>
#include <filesystem>

namespace fusionmcp::runtime {
fusionframework::config::Document LoadConfig() {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadConfig),&module)) return {};
    std::array<wchar_t,32768> path{};
    const auto length=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
    if (!length||length>=path.size()) return {};
    const auto file=std::filesystem::path(path.data()).parent_path()/L"FusionMCP.toml";
    const auto text=fusionframework::config::ReadFile(file);
    if (!text) return {};
    std::string error;
    auto document=fusionframework::config::Document::TryParse(*text,&error,"FusionMCP.toml");
    if (!error.empty()) REX::WARN("FusionMCP.toml is not valid TOML; using defaults\n{}",error);
    return document;
}
}
