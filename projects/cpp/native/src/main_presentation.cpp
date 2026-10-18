#include "sonyconnect/main_window.hpp"
#include <algorithm>

namespace sonyconnect::ui {
std::optional<MainFixationReadback::Context> MainFixationReadback::next(
    const DeviceSnapshot &state, std::uint64_t now_ms) {
    if (!state.ready || !state.sources.available || !state.sources.multipoint.known() ||
        !*state.sources.multipoint.value || !state.sources.active_source.known()) {
        context_ = {}; attempts_ = 0; due_ = 0;
        return {};
    }
    Context current{state.link, *state.sources.active_source.value};
    if (current.link != context_.link || current.source != context_.source) {
        context_ = current; attempts_ = 0; due_ = 0;
    }
    if (state.touch.playback_fixed.known()) {
        attempts_ = 0; due_ = 0;
        return {};
    }
    const auto feature = state.features.find(protocol::Feature::FixedPlayback);
    if (state.busy || feature == state.features.end() || !feature->second.advertised ||
        attempts_ >= 2 || now_ms < due_) return {};
    ++attempts_;
    due_ = now_ms + 1500;
    return current;
}
std::wstring main_media_source(const MainModel &model) {
    for (const auto &row : model.source_rows)
        if (row.connected && row.active && !row.label.empty())
            return L"来自" + row.label;
    for (const auto &row : model.source_rows)
        if (row.windows_endpoint && row.connected)
            return L"来自本机";
    return L"音源待回读";
}
void project_main_sources(MainModel &model, bool endpoint_current) {
    auto text = [](const std::string &input) {
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                             static_cast<int>(input.size()), nullptr, 0);
        std::wstring output(size, L' ');
        if (size)
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                                output.data(), size);
        return output;
    };
    const auto &device = model.device;
    const bool single = device.sources.multipoint.known() && !*device.sources.multipoint.value;
    const auto capacity = single ? L"1" : L"2";
    const auto connected_entries =
        std::count_if(device.sources.entries.begin(), device.sources.entries.end(),
                      [](const auto &source) { return source.connection_slot != 0; });
    // During a multipoint transition its setting can arrive before the refreshed
    // list. Never present an old two-device list as a confirmed "2 / 1".
    const bool current = device.ready && device.sources.available && (!single || connected_entries <= 1);
    const bool writable = device.ready && !device.busy && !model.busy && !model.preview;
    model.source_rows.clear();
    model.source_summary.clear();
    model.source_count = std::wstring(L"— / ") + capacity;
    int protocol_index = 0;
    for (const auto &source : device.sources.entries) {
        if (!source.connection_slot)
            continue;
        MainSourceRow row;
        row.label = text(source.name);
        row.category = protocol::AudioSourceParameters::category(source.class_of_device);
        row.connected = current;
        row.protocol_index = current ? protocol_index : -1;
        row.active = current && device.sources.active_source.known() &&
                     *device.sources.active_source.value == source.id;
        const bool fixed =
            row.active && device.touch.playback_fixed.known() && *device.touch.playback_fixed.value;
        row.status = !current     ? L"上次连接 · 待回读"
                     : fixed      ? L"已固定播放"
                     : row.active ? L"当前音源"
                                  : L"已连接";
        row.can_switch = writable && current && device.sources.switch_writable && !row.active;
        row.can_fix = writable && row.active && device.touch.fixation_writable;
        model.source_rows.push_back(std::move(row));
        ++protocol_index;
    }
    if (current && protocol_index > 0) {
        model.source_count = std::to_wstring(protocol_index) + L" / " + capacity;
        return;
    }
    // An empty SourceSnapshot has no freshness/known-empty bit. Do not invent a
    // zero connection count or insert Windows evidence into protocol SourceEntry.
    if (device.ready && single && model.source_rows.empty() && endpoint_current &&
        model.local.volume.can_write && !model.local.volume.endpoint_id.empty()) {
        MainSourceRow local;
        local.label = L"本机 · Windows 音频端点";
        local.status = L"本机输出 · 音源待回读";
        local.category = protocol::SourceCategory::Computer;
        local.windows_endpoint = true;
        local.connected = true;
        model.source_rows.push_back(std::move(local));
        model.source_summary = L"Windows 已确认此耳机为本机输出；耳机音源列表尚未回读。";
    } else {
        model.source_summary =
            model.source_rows.empty() ? L"耳机音源列表尚未确认。" : L"显示上次连接记录，等待耳机回读。";
    }
}
} // namespace sonyconnect::ui
