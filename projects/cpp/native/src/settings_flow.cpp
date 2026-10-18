#include "sonyconnect/settings_flow.hpp"
#include <algorithm>
#include <exception>

namespace sonyconnect::ui {
namespace {
bool assistant_allowed(const DeviceSnapshot &d, int v) {
    const auto &c = d.touch.voice_assistant_candidates;
    return d.touch.assistant_writable && std::find(c.begin(), c.end(), v) != c.end();
}
bool touch_allowed(const DeviceSnapshot &d, int key, int preset) {
    const auto it = std::find_if(d.touch.keys.begin(), d.touch.keys.end(),
                                  [&](const auto &k) { return k.id == key; });
    return d.touch.writable && it != d.touch.keys.end() &&
           std::any_of(it->presets.begin(), it->presets.end(),
                       [&](const auto &p) { return p.id == preset; });
}
bool has_playback(const SettingsContext &c) {
    return std::find(c.assignments.begin(), c.assignments.end(), 0x20) != c.assignments.end();
}
bool apply(SettingsContext &c, const SettingsStep &s) {
    if (s.kind == SettingsStepKind::assistant) { c.assistant = s.value; return true; }
    const auto key = std::find(c.keys.begin(), c.keys.end(), s.key);
    if (key == c.keys.end()) return false;
    c.assignments[static_cast<std::size_t>(key - c.keys.begin())] = s.value;
    return true;
}
SettingsPlanResult unavailable(const char *reason) { return {{}, {}, reason}; }
}
std::optional<SettingsContext> settings_context(const DeviceSnapshot &d) {
    if (!d.ready || !d.device || !d.touch.assignments.known() || !d.touch.voice_assistant.known() ||
        d.touch.assignments.value->size() != d.touch.keys.size()) return {};
    SettingsContext c{d.link, d.device->address, d.device->aep_id, d.model, {},
                      *d.touch.assignments.value, *d.touch.voice_assistant.value};
    for (const auto &key : d.touch.keys) c.keys.push_back(key.id);
    return c;
}
SettingsPlanResult plan_mobile_assistant(const DeviceSnapshot &d, protocol::Byte value,
                                         std::optional<protocol::Byte> key) {
    const auto before = settings_context(d);
    if (!before || d.model != "WF-1000XM5" || !assistant_allowed(d, value))
        return unavailable("助手或触摸状态尚未确认，无法提交关联修改。");
    LinkedSettingsPlan plan{*before, {}};
    if (value == 0x30 && !has_playback(*before)) {
        protocol::Bytes candidates;
        for (const auto &k : d.touch.keys)
            if (k.id <= 1 && touch_allowed(d, k.id, 0x20)) candidates.push_back(k.id);
        if (candidates.empty()) return unavailable("耳机未上报可分配播放控制的耳侧。");
        if (!key) return {{}, candidates, {}};
        if (std::find(candidates.begin(), candidates.end(), *key) == candidates.end())
            return unavailable("所选耳侧不支持播放控制。");
        plan.steps.push_back({SettingsStepKind::touch, *key, 0x20});
    }
    if (before->assistant != value) plan.steps.push_back({SettingsStepKind::assistant, 0, value});
    return {plan, {}, {}};
}
SettingsPlanResult plan_touch_assignment(const DeviceSnapshot &d, protocol::Byte key, protocol::Byte preset) {
    const auto before = settings_context(d);
    if (!before || d.model != "WF-1000XM5" || !touch_allowed(d, key, preset))
        return unavailable("触摸分配或助手状态尚未确认，无法提交关联修改。");
    LinkedSettingsPlan plan{*before, {}};
    SettingsStep touch{SettingsStepKind::touch, key, preset};
    auto after = *before;
    if (!apply(after, touch)) return unavailable("所选耳侧已变化。");
    if (before->assignments == after.assignments) return {plan, {}, {}};
    if (before->assistant == 0x30 && has_playback(*before) && !has_playback(after)) {
        if (!assistant_allowed(d, 0xff)) return unavailable("耳机未确认可关闭语音助手，本次修改未提交。");
        plan.steps.push_back({SettingsStepKind::assistant, 0, 0xff});
    }
    plan.steps.push_back(touch);
    return {plan, {}, {}};
}
SettingsPlanResult plan_linked_restore(const DeviceSnapshot &d, const LinkedSettingsBackup &backup) {
    const auto current = settings_context(d);
    if (!current || *current != backup.after || current->model != "WF-1000XM5" ||
        backup.before.keys != current->keys || backup.before.link != current->link ||
        backup.before.address != current->address || backup.before.endpoint != current->endpoint ||
        backup.before.model != current->model || backup.before.assignments.size() != current->keys.size())
        return unavailable("关联配置已被更改或连接已变化，原备份不再适用。");
    if (backup.before.assistant == 0x30 && !has_playback(backup.before))
        return unavailable("原配置没有播放控制，不能恢复为已启用的移动设备语音助手。");
    LinkedSettingsPlan plan{*current, {}};
    const bool touch_changes = current->assignments != backup.before.assignments;
    if (touch_changes && current->assistant != 0xff) {
        if (!assistant_allowed(d, 0xff)) return unavailable("耳机不允许先关闭助手，无法恢复触摸配置。");
        plan.steps.push_back({SettingsStepKind::assistant, 0, 0xff});
    }
    for (std::size_t i = 0; i < current->keys.size(); ++i)
        if (current->assignments[i] != backup.before.assignments[i]) {
            if (!touch_allowed(d, current->keys[i], backup.before.assignments[i]))
                return unavailable("原触摸分配不在当前耳机上报的候选中。");
            plan.steps.push_back({SettingsStepKind::touch, current->keys[i], backup.before.assignments[i]});
        }
    if (touch_changes || current->assistant != backup.before.assistant) {
        if (!assistant_allowed(d, backup.before.assistant)) return unavailable("原语音助手不在当前候选中。");
        plan.steps.push_back({SettingsStepKind::assistant, 0, static_cast<protocol::Byte>(backup.before.assistant)});
    }
    return {plan, {}, {}};
}
SettingsFlowResult run_settings_flow(const LinkedSettingsPlan &plan,
                                    const std::function<DeviceSnapshot()> &read,
                                    const std::function<Result(const SettingsStep &)> &write) {
    SettingsFlowResult out;
    auto expected = plan.before;
    auto final = expected;
    for (const auto &step : plan.steps)
        if (!apply(final, step)) {
            out.result = {ResultCode::InvalidValue, "关联计划中的耳侧不再有效。"};
            return out;
        }
    const auto capture_backup = [&](const SettingsContext &actual) {
        if (actual != plan.before) out.backup = LinkedSettingsBackup{plan.before, actual};
    };
    if (plan.steps.empty()) {
        if (settings_context(read()) != std::optional<SettingsContext>{expected})
            out.result = {ResultCode::Canceled, "设备连接或配置已变化，关联修改已停止。"};
        return out;
    }
    for (const auto &step : plan.steps) {
        const auto snapshot = read();
        const auto current = settings_context(snapshot);
        if (!current || *current != expected) {
            out.result = {ResultCode::Canceled, "设备连接或配置已变化，关联修改已停止。"};
            return out;
        }
        if (step.kind == SettingsStepKind::assistant ? !assistant_allowed(snapshot, step.value)
                                                     : !touch_allowed(snapshot, step.key, step.value)) {
            out.result = {ResultCode::Unavailable, "耳机可写能力已变化，关联修改已停止。"};
            return out;
        }
        auto target = expected;
        apply(target, step);
        Result result;
        try { result = write(step); }
        catch (const std::exception &e) { result = {ResultCode::Transport, e.what()}; }
        catch (...) { result = {ResultCode::Transport, "关联设置操作发生异常。"}; }
        const auto actual = settings_context(read());
        if (actual && (*actual == expected || *actual == target || *actual == final)) capture_backup(*actual);
        if (!result.confirmed()) {
            out.result = {result.ok() ? ResultCode::DeviceRejected : result.code,
                          "关联修改未完成，请以实际回读为准。" + result.message};
            return out;
        }
        ++out.completed;
        // A controller may itself restore a saved assistant. Accept an early
        // final state only when every requested value is actually confirmed.
        if (actual && *actual == final) { out.result = {}; return out; }
        if (!actual || *actual != target) {
            out.result = {ResultCode::DeviceRejected, "关联设置回读与期望不符，已停止后续操作。"};
            return out;
        }
        expected = target;
    }
    out.result = {};
    return out;
}
std::optional<LinkedSettingsBackup> merge_settings_backup(
    std::optional<LinkedSettingsBackup> current, const LinkedSettingsPlan &plan,
    const SettingsFlowResult &flow, bool restoring) {
    if (restoring && flow.result.confirmed()) return {};
    if (!flow.backup) return current;
    auto original = flow.backup->before;
    if (current && current->after == plan.before) original = current->before;
    if (original == flow.backup->after) return {};
    return LinkedSettingsBackup{original, flow.backup->after};
}
} // namespace sonyconnect::ui
