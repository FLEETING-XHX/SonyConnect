#pragma once
#include <string>
#include <vector>

namespace sonyconnect::desktop {
struct EmbeddedNotice { std::string name, text; };
std::string embedded_update_json();
std::vector<EmbeddedNotice> embedded_notices();
} // namespace sonyconnect::desktop
