#pragma once
#include "sonyconnect/session.hpp"
#include <functional>

namespace sonyconnect::ui {
// Application-level sequencing of existing controller operations. No wire data
// is encoded here. Context includes key order to reject stale ear assignments.
struct SettingsContext {
    std::uint64_t link{}, address{};
    std::wstring endpoint;
    std::string model;
    protocol::Bytes keys, assignments;
    int assistant{};
    bool operator==(const SettingsContext &) const = default;
};
enum class SettingsStepKind { touch, assistant };
struct SettingsStep {
    SettingsStepKind kind{};
    protocol::Byte key{}, value{};
};
struct LinkedSettingsPlan {
    SettingsContext before;
    std::vector<SettingsStep> steps;
};
struct LinkedSettingsBackup { SettingsContext before, after; };
struct SettingsPlanResult {
    std::optional<LinkedSettingsPlan> plan;
    protocol::Bytes playback_keys;
    std::string reason;
};
struct SettingsFlowResult {
    Result result;
    unsigned completed{};
    std::optional<LinkedSettingsBackup> backup;
};
std::optional<SettingsContext> settings_context(const DeviceSnapshot &);
SettingsPlanResult plan_mobile_assistant(const DeviceSnapshot &, protocol::Byte,
                                         std::optional<protocol::Byte> playback_key = {});
SettingsPlanResult plan_touch_assignment(const DeviceSnapshot &, protocol::Byte, protocol::Byte);
SettingsPlanResult plan_linked_restore(const DeviceSnapshot &, const LinkedSettingsBackup &);
SettingsFlowResult run_settings_flow(const LinkedSettingsPlan &,
                                    const std::function<DeviceSnapshot()> &read,
                                    const std::function<Result(const SettingsStep &)> &write);
std::optional<LinkedSettingsBackup> merge_settings_backup(
    std::optional<LinkedSettingsBackup>, const LinkedSettingsPlan &, const SettingsFlowResult &, bool restoring);
} // namespace sonyconnect::ui
