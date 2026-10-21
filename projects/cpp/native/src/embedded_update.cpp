#include "sonyconnect/desktop.hpp"
#include "sonyconnect/embedded_assets.hpp"
namespace sonyconnect::desktop {
UpdateSource UpdateSource::embedded() { return parse(embedded_update_json()); }
}
