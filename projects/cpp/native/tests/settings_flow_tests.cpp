#include "sonyconnect/settings_flow.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace sc = sonyconnect;
namespace ui = sc::ui;
namespace p = sc::protocol;
namespace {
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
template<class T> sc::Value<T> known(T value) {
    sc::Value<T> out;
    out.status = sc::ValueStatus::Known;
    out.value = std::move(value);
    return out;
}
sc::DeviceSnapshot fixture() {
    sc::DeviceSnapshot d;
    d.ready = d.connected = true;
    d.model = "WF-1000XM5";
    d.link = 9;
    d.device = sc::DeviceDescriptor{};
    d.device->address = 0x123;
    d.device->aep_id = L"fixture-headphone";
    d.touch.keys = {{0, 0, 0, {{0}, {0x20}, {0xff}}}, {1, 0, 0, {{0}, {0x20}, {0xff}}}};
    d.touch.assignments = known(p::Bytes{0, 0});
    d.touch.voice_assistant = known(0xff);
    d.touch.voice_assistant_candidates = {0x30, 0xff};
    d.touch.writable = d.touch.assistant_writable = true;
    return d;
}
void apply(sc::DeviceSnapshot &d, const ui::SettingsStep &s) {
    if (s.kind == ui::SettingsStepKind::assistant) d.touch.voice_assistant = known<int>(s.value);
    else {
        const auto key = std::find_if(d.touch.keys.begin(), d.touch.keys.end(), [&](auto &k) { return k.id == s.key; });
        check(key != d.touch.keys.end(), "Writer must target actual key");
        (*d.touch.assignments.value)[key - d.touch.keys.begin()] = s.value;
    }
}
}
int main() {
    unsigned passed{};
    try {
        auto d = fixture();
        const auto choices = ui::plan_mobile_assistant(d, 0x30);
        check(!choices.plan && choices.playback_keys == p::Bytes({0, 1}), "No playback must offer reported physical ears");
        ++passed;
        auto enable = ui::plan_mobile_assistant(d, 0x30, p::Byte{1});
        check(enable.plan && enable.plan->steps.size() == 2 && enable.plan->steps[0].kind == ui::SettingsStepKind::touch &&
              enable.plan->steps[0].key == 1 && enable.plan->steps[0].value == 0x20 && enable.plan->steps[1].value == 0x30,
              "Enable must assign playback before enabling mobile assistant");
        unsigned calls{};
        auto completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &s) {
            ++calls; apply(d, s); return sc::Result{};
        });
        check(completed.result.confirmed() && calls == 2 && completed.backup && d.touch.voice_assistant.value == 0x30 &&
                  *d.touch.assignments.value == p::Bytes({0, 0x20}), "Flow must confirm both writes and retain exact backup");
        ++passed;
        const auto backup = *completed.backup;
        auto remove = ui::plan_touch_assignment(d, 1, 0);
        check(remove.plan && remove.plan->steps.size() == 2 && remove.plan->steps[0].kind == ui::SettingsStepKind::assistant &&
                  remove.plan->steps[0].value == 0xff && remove.plan->steps[1].value == 0,
              "Last playback removal must disable assistant before changing assignment");
        completed = ui::run_settings_flow(*remove.plan, [&] { return d; }, [&](const auto &s) { apply(d, s); return sc::Result{}; });
        check(completed.result.confirmed() && d.touch.voice_assistant.value == 0xff, "Removal must read back actual disabled assistant");
        check(!ui::merge_settings_backup(backup, *remove.plan, completed, false),
              "Returning to original values must not retain a redundant backup");
        ++passed;
        d = fixture();
        d.touch.assignments = known(p::Bytes{0x20, 0x20});
        d.touch.voice_assistant = known(0x30);
        const auto retain = ui::plan_touch_assignment(d, 0, 0);
        check(retain.plan && retain.plan->steps.size() == 1 && retain.plan->steps[0].kind == ui::SettingsStepKind::touch,
              "A remaining playback assignment must preserve assistant in the derived plan");
        ++passed;
        d = fixture();
        d.touch.keys[0].presets = {{0}, {0xff}};
        check(ui::plan_mobile_assistant(d, 0x30).playback_keys == p::Bytes({1}) &&
                  !ui::plan_mobile_assistant(d, 0x30, p::Byte{0}).plan, "Unadvertised playback ear must never be offered or written");
        ++passed;
        d = fixture();
        d.touch.assignments = {};
        check(!ui::plan_mobile_assistant(d, 0x30).plan && !ui::plan_touch_assignment(d, 1, 0).plan,
              "Unknown assignments must not be treated as off");
        d = fixture();
        d.touch.voice_assistant = {};
        check(!ui::plan_mobile_assistant(d, 0x30).plan, "Unknown assistant must block linked plan");
        ++passed;
        d = fixture();
        ++d.link;
        calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](auto &) { ++calls; return sc::Result{}; });
        check(completed.result.code == sc::ResultCode::Canceled && calls == 0, "Reconnect must reject old dialog intent before writing");
        ++passed;
        d = fixture();
        std::swap(d.touch.keys[0], d.touch.keys[1]);
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](auto &) { ++calls; return sc::Result{}; });
        check(completed.result.code == sc::ResultCode::Canceled && calls == 0, "Key reorder must invalidate positional assignments");
        ++passed;
        d = fixture(); calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &s) {
            ++calls; apply(d, s); (*d.touch.assignments.value)[0] = 0xff; return sc::Result{};
        });
        check(!completed.result.confirmed() && calls == 1 && !completed.backup,
              "Unexpected parallel assignment changes must stop and must not become a restorable app write");
        ++passed;
        d = fixture(); calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &s) {
            if (++calls == 2) return sc::Result{sc::ResultCode::DeviceRejected, "fixture rejection"};
            apply(d, s); return sc::Result{};
        });
        check(!completed.result.ok() && completed.completed == 1 && completed.backup &&
                  d.touch.voice_assistant.value == 0xff && (*d.touch.assignments.value)[1] == 0x20,
              "Second step failure must retain confirmed partial state and backup without pretending rollback");
        ++passed;
        const auto partial_backup = *completed.backup;
        auto restore = ui::plan_linked_restore(d, partial_backup);
        check(restore.plan.has_value(), "Confirmed partial state must be explicitly restorable");
        completed = ui::run_settings_flow(*restore.plan, [&] { return d; }, [&](const auto &s) { apply(d, s); return sc::Result{}; });
        check(completed.result.confirmed() && ui::settings_context(d) == partial_backup.before,
              "Restore must read back the original configuration");
        ++passed;
        d = fixture(); calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &) {
            ++calls; return sc::Result{sc::ResultCode::Delivered, "not confirmed"};
        });
        check(!completed.result.ok() && completed.completed == 0 && calls == 1 && !completed.backup,
              "Delivered without confirmation must never start next step");
        ++passed;
        d = fixture(); calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &s) {
            ++calls; apply(d, s); return sc::Result{sc::ResultCode::DeviceRejected, "write took effect before error"};
        });
        check(!completed.result.ok() && completed.backup && calls == 1 && (*d.touch.assignments.value)[1] == 0x20,
              "Error after actual change must preserve actual readback and permit explicit restore");
        ++passed;
        d = fixture(); calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &s) -> sc::Result {
            if (++calls == 2) throw std::runtime_error("fixture I/O failure");
            apply(d, s); return {};
        });
        check(completed.result.code == sc::ResultCode::Transport && completed.backup && completed.completed == 1,
              "Exceptions must retain first confirmed step for recovery");
        ++passed;
        d = fixture();
        d.touch.assignments = known(backup.after.assignments);
        d.touch.voice_assistant = known(backup.after.assistant);
        check(ui::plan_linked_restore(d, backup).plan.has_value(), "Intact backup must be restorable");
        (*d.touch.assignments.value)[0] = 0xff;
        check(!ui::plan_linked_restore(d, backup).plan, "Phone changes must invalidate backup instead of being overwritten");
        ++passed;
        d = fixture(); d.link++;
        check(!ui::plan_linked_restore(d, backup).plan, "Backup must not cross a device connection");
        ++passed;
        d = fixture(); d.model = "OtherModel";
        check(!ui::plan_mobile_assistant(d, 0x30).plan, "WF-specific dependencies must not be generalized to other models");
        ++passed;
        d = fixture(); calls = 0;
        completed = ui::run_settings_flow(*enable.plan, [&] { return d; }, [&](const auto &s) {
            ++calls; apply(d, s); d.touch.voice_assistant = known(0x30); return sc::Result{};
        });
        check(completed.result.confirmed() && calls == 1 && completed.backup,
              "An already confirmed final state must not send redundant assistant writes");
        ++passed;
        d = fixture();
        d.touch.assignments = known(backup.after.assignments);
        d.touch.voice_assistant = known(backup.after.assistant);
        restore = ui::plan_linked_restore(d, backup);
        calls = 0;
        completed = ui::run_settings_flow(*restore.plan, [&] { return d; }, [&](const auto &s) {
            if (++calls == 2) return sc::Result{sc::ResultCode::DeviceRejected, "restore touch rejected"};
            apply(d, s); return sc::Result{};
        });
        auto retained = ui::merge_settings_backup(backup, *restore.plan, completed, true);
        check(!completed.result.ok() && retained && retained->before == backup.before &&
                  ui::settings_context(d) == retained->after,
              "Partial restore must retain original goal with updated confirmed current values");
        restore = ui::plan_linked_restore(d, *retained);
        check(restore.plan.has_value(), "Partial restore must permit explicit retry from actual state");
        completed = ui::run_settings_flow(*restore.plan, [&] { return d; }, [&](const auto &s) { apply(d, s); return sc::Result{}; });
        check(completed.result.confirmed() && !ui::merge_settings_backup(retained, *restore.plan, completed, true),
              "Successful restore must clear only its application backup");
        ++passed;
        std::cout << passed << " linked settings integration scenarios passed; HardwareVerified=false; BluetoothApisCalled=false\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << passed << " passed before failure: " << e.what() << '\n';
        return 1;
    }
}
