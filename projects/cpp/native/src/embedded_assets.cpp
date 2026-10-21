#include "sonyconnect/embedded_assets.hpp"
#include "embedded_asset_index.hpp"
#include <windows.h>
#include <stdexcept>

namespace sonyconnect::desktop {
namespace {
std::string resource_text(int id) {
    const auto module = GetModuleHandleW(nullptr);
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) throw std::runtime_error("Missing embedded distribution resource");
    const auto size = SizeofResource(module, resource);
    const auto data = LockResource(LoadResource(module, resource));
    if (!data || !size) throw std::runtime_error("Empty embedded distribution resource");
    return {static_cast<const char *>(data), size};
}
}
std::string embedded_update_json() { return resource_text(201); }
std::vector<EmbeddedNotice> embedded_notices() {
    std::vector<EmbeddedNotice> out;
    for (const auto &entry : embedded_notice_index)
        out.push_back({entry.name, resource_text(entry.id)});
    return out;
}
} // namespace sonyconnect::desktop
