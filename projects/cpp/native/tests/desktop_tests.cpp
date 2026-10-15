#include "sonyconnect/desktop.hpp"
#include <winrt/base.h>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace sonyconnect::desktop;
namespace {
void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
template <class F> void rejects(F f, const char *message) {
    bool rejected = false;
    try {
        f();
    } catch (const std::exception &) {
        rejected = true;
    } catch (const winrt::hresult_error &) {
        rejected = true;
    }
    require(rejected, message);
}
void write(const std::filesystem::path &path, const std::string &data) {
    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), data.size());
    require(static_cast<bool>(out), "Fixture write failed");
}
std::string read(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
std::string unhex(const std::string &hex) {
    std::string result;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        result.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return result;
}
bool same_bindings(const HotkeyBindings &left, const HotkeyBindings &right) {
    for (std::size_t i = 0; i < left.size(); ++i)
        if (left[i].mode != right[i].mode || left[i].gesture != right[i].gesture)
            return false;
    return true;
}
struct FakeHotkeyPlatform {
    std::optional<HotkeyBindings> occupied;
    std::vector<HotkeyBindings> probed;
    bool start_fails{}, active{};
    unsigned start_calls{}, stop_calls{};
    detail::HotkeyPlatform callbacks() {
        return {[this](const HotkeyBindings &bindings) {
                    probed.push_back(bindings);
                    if (occupied && same_bindings(*occupied, bindings))
                        throw std::runtime_error("Simulated occupied hotkey");
                },
                [this] {
                    ++start_calls;
                    if (start_fails)
                        throw std::runtime_error("Simulated hook installation failure");
                    active = true;
                },
                [this] {
                    ++stop_calls;
                    active = false;
                }};
    }
};
// Real .NET ZipArchive CompressionLevel.Optimal output: fixed and dynamic DEFLATE.
const std::string legacy_fixture = unhex(
    "504B0304140000000800B6604E5D0E9112EB31000000290000000D0000006D616E69666573742E6A736F6EAA560A4ECE48CD4D54"
    "B2520ACECFAB74CECFCB4B4D2ED173C94C4CCFCB2F2EC94C2ED62B3354AAE5E502000000FFFF0300"
    "504B0304140000000800B6604E5D430350778A0000009E000000140000006C6F67732F72756E2D6F6C642D302E6A736F6E6C2488"
    "B10AC24010057BC19F78AD89EC5D128B05CB804D2A23DA86DC16A2D993DCAA48C8BF8B71986666427B1D2459373CC0F0E477B9A3"
    "DC952D112F6EE9CF662964A8D5C60F7842FD123530FAA82ABD494086A375F64C6074218C92D29E889D63EFB928B82CB9AA90A189"
    "41EE609C0FB923A24BF39B27BD697C2B1849FA510CF3BC5E7D010000FFFF0300"
    "504B01021400140000000800B6604E5D0E9112EB31000000290000000D00000000000000000000000000000000006D616E696665"
    "73742E6A736F6E504B01021400140000000800B6604E5D430350778A0000009E00000014000000000000000000000000005C0000"
    "006C6F67732F72756E2D6F6C642D302E6A736F6E6C504B050600000000020002007D000000180100000000");
} // namespace
int main() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto temp = std::filesystem::temp_directory_path() /
                    (L"SonyConnect-native-desktop-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                     std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(temp);
        auto defaults = default_hotkeys();
        validate_hotkeys(defaults);
        HotkeyMatcher matcher;
        matcher.feed(VK_LCONTROL, true, false, false, defaults);
        require(!matcher.feed(VK_F1, true, false, false, defaults).mode,
                "Left control must not trigger right control binding");
        matcher.reset();
        matcher.feed(VK_RCONTROL, true, false, false, defaults);
        auto first = matcher.feed(VK_F1, true, false, false, defaults);
        require(first.mode == SoundMode::noise_canceling && first.consume, "Default chord missing");
        require(!matcher.feed(VK_F1, true, false, false, defaults).mode, "Repeat must not retrigger");
        require(matcher.feed(VK_F1, false, false, false, defaults).consume,
                "Matched key-up must be consumed");
        matcher.reset();
        matcher.feed(VK_RCONTROL, true, true, false, defaults);
        require(!matcher.feed(VK_F1, true, false, false, defaults).mode,
                "Injected modifier must not activate chord");
        matcher.reset();
        matcher.feed(VK_RCONTROL, true, false, true, defaults);
        require(!matcher.feed(VK_F1, true, false, true, defaults).mode, "Paused listener must not dispatch");
        HotkeyCapture capture;
        capture.begin();
        capture.feed(VK_RCONTROL, true);
        capture.feed(VK_LSHIFT, true);
        require(!capture.feed('A', true).gesture, "Multiple recognizers cannot be captured");
        capture.end();
        capture.begin();
        capture.feed(VK_TAB, true);
        auto captured = capture.feed('A', true);
        require(captured.gesture && captured.finished, "Tab recognizer capture failed");
        auto invalid = defaults;
        invalid[1].gesture = invalid[0].gesture;
        rejects([&] { validate_hotkeys(invalid); }, "Duplicate bindings accepted");
        require(!is_free_key(VK_BACK) && !is_free_key(VK_DELETE) && !is_free_key(VK_SNAPSHOT) &&
                    !is_free_key(VK_LWIN),
                "Forbidden keys accepted");
        HotkeyStore store(temp / L"hotkeys.json");
        store.save(defaults);
        auto loaded = store.load();
        require(loaded[2].gesture == defaults[2].gesture, "Hotkey persistence changed gesture");
        auto changed = defaults;
        changed[2].gesture = {Recognizer::tab, 'Z'};
        store.save(changed);
        require(std::filesystem::exists(temp / L"hotkeys.json.bak"), "Previous settings backup missing");
        auto future = std::string("{\"Version\":2,\"Bindings\":[]}");
        write(temp / L"future.json", future);
        HotkeyStore future_store(temp / L"future.json");
        future_store.load();
        rejects([&] { future_store.save(defaults); }, "Unknown version overwritten");
        require(read(temp / L"future.json") == future, "Future settings changed");
        auto fractional = read(temp / L"hotkeys.json");
        auto key_offset = fractional.find("\"Key\":");
        require(key_offset != std::string::npos, "Missing fixture key");
        auto number_end = fractional.find_first_of(",}", key_offset + 6);
        fractional.replace(key_offset + 6, number_end - key_offset - 6, "112.5");
        write(temp / L"fractional.json", fractional);
        HotkeyStore fractional_store(temp / L"fractional.json");
        fractional_store.load();
        require(!fractional_store.warning().empty(), "Fractional virtual key silently rounded");
        require(read(temp / L"fractional.json") == fractional, "Invalid settings were rewritten");
        auto replacement = defaults;
        replacement[0].gesture.key = VK_F4;
        replacement[1].gesture.key = VK_F5;
        replacement[2].gesture.key = VK_F6;
        auto recovery_path = temp / L"hotkey-recovery.json";
        HotkeyStore recovery_store(recovery_path);
        recovery_store.save(defaults);
        auto original_settings = read(recovery_path);
        FakeHotkeyPlatform recovery_platform;
        recovery_platform.occupied = defaults;
        {
            GlobalHotkeys listener(recovery_path, [](SoundMode) {}, recovery_platform.callbacks());
            rejects([&] { listener.start(); }, "Occupied previous bindings should fail startup");
            require(recovery_platform.start_calls == 0 && read(recovery_path) == original_settings,
                    "Failed startup changed settings or installed a listener");
            listener.save(replacement);
            require(recovery_platform.probed.size() == 2 &&
                        same_bindings(recovery_platform.probed.back(), replacement) &&
                        recovery_platform.start_calls == 1,
                    "Saving replacement must not probe occupied previous bindings again");
            require(same_bindings(listener.bindings(), replacement) &&
                        same_bindings(recovery_store.load(), replacement),
                    "Replacement did not become active and persistent after conflict recovery");
            require(read(recovery_path.wstring() + L".bak") == original_settings,
                    "Conflict recovery did not retain the previous settings backup");
            listener.start();
            require(recovery_platform.start_calls == 1 && recovery_platform.probed.size() == 2,
                    "Starting an active listener must not reprobe or reinstall it");
            auto saved = read(recovery_path);
            auto old_status = listener.status();
            rejects([&] { listener.save(defaults); }, "Occupied candidate was accepted");
            require(same_bindings(listener.bindings(), replacement) && read(recovery_path) == saved &&
                        listener.status() == old_status && recovery_platform.start_calls == 1,
                    "Rejected candidate changed active bindings, status, persistence or listener");
            auto probes = recovery_platform.probed.size();
            rejects([&] { listener.save(invalid); }, "Invalid candidate was accepted");
            require(recovery_platform.probed.size() == probes && read(recovery_path) == saved &&
                        same_bindings(listener.bindings(), replacement),
                    "Invalid candidate reached platform side effects or changed settings");
        }
        require(recovery_platform.stop_calls == 1, "Recovered listener was not released exactly once");
        auto hook_failure_path = temp / L"hotkey-hook-failure.json";
        HotkeyStore(hook_failure_path).save(defaults);
        auto before_hook_failure = read(hook_failure_path);
        FakeHotkeyPlatform hook_failure_platform;
        hook_failure_platform.start_fails = true;
        {
            GlobalHotkeys listener(hook_failure_path, [](SoundMode) {}, hook_failure_platform.callbacks());
            rejects([&] { listener.save(replacement); }, "Hook installation failure was ignored");
            require(same_bindings(listener.bindings(), defaults) &&
                        read(hook_failure_path) == before_hook_failure,
                    "Failed hook installation activated or persisted uninstalled bindings");
            hook_failure_platform.start_fails = false;
            listener.save(replacement);
            require(hook_failure_platform.start_calls == 2 && same_bindings(listener.bindings(), replacement),
                    "Failed listener installation could not be retried with the candidate");
        }
        require(hook_failure_platform.stop_calls == 1, "Failed installation produced a stale listener");
        FakeHotkeyPlatform persistence_failure_platform;
        {
            GlobalHotkeys listener(
                temp / L"future.json", [](SoundMode) {}, persistence_failure_platform.callbacks());
            auto before_status = listener.status();
            rejects([&] { listener.save(replacement); }, "Persistence refusal was ignored");
            require(same_bindings(listener.bindings(), defaults) && read(temp / L"future.json") == future &&
                        listener.status() == before_status,
                    "Persistence failure changed previous bindings, settings file or status");
            require(persistence_failure_platform.stop_calls == 1 && !persistence_failure_platform.active,
                    "Persistence failure did not immediately undo the newly started listener");
        }
        require(persistence_failure_platform.stop_calls == 1,
                "Listener cleanup was lost after a settings write failure");
        FakeHotkeyPlatform running_failure_platform;
        {
            GlobalHotkeys listener(
                temp / L"future.json", [](SoundMode) {}, running_failure_platform.callbacks());
            listener.start();
            rejects([&] { listener.save(replacement); }, "Active listener ignored persistence refusal");
            require(running_failure_platform.active && running_failure_platform.stop_calls == 0 &&
                        same_bindings(listener.bindings(), defaults) && read(temp / L"future.json") == future,
                    "Persistence failure stopped an existing listener or changed its previous bindings");
        }
        require(running_failure_platform.stop_calls == 1 && !running_failure_platform.active,
                "Existing listener was not released on destruction");
        auto retry_path = temp / L"hotkey-write-retry.json";
        FakeHotkeyPlatform retry_platform;
        {
            GlobalHotkeys listener(retry_path, [](SoundMode) {}, retry_platform.callbacks());
            // The fixture belongs to this exclusively created test directory. An empty
            // directory at the file path makes ReplaceFile fail without changing OS permissions.
            std::filesystem::create_directory(retry_path);
            rejects([&] { listener.save(replacement); }, "Unwritable settings path was accepted");
            require(!retry_platform.active && retry_platform.start_calls == 1 &&
                        retry_platform.stop_calls == 1 && same_bindings(listener.bindings(), defaults) &&
                        std::filesystem::is_directory(retry_path),
                    "Write failure did not restore the inactive listener and previous settings");
            require(std::filesystem::remove(retry_path), "Exclusive write failure fixture cleanup failed");
            listener.save(replacement);
            require(retry_platform.active && retry_platform.start_calls == 2 &&
                        retry_platform.stop_calls == 1 && same_bindings(listener.bindings(), replacement) &&
                        same_bindings(HotkeyStore(retry_path).load(), replacement),
                    "Saving a valid candidate could not retry after persistence rollback");
        }
        require(retry_platform.stop_calls == 2 && !retry_platform.active,
                "Retried listener was not released exactly once");
        require(!ReleaseVersion::parse("1.02.3") && !ReleaseVersion::parse("1.2.3-01") &&
                    !ReleaseVersion::parse("1.2.2147483648"),
                "Invalid SemVer accepted");
        auto stable = ReleaseVersion::parse("v1.2.3+metadata"), rc = ReleaseVersion::parse("1.2.3-rc.9"),
             rc10 = ReleaseVersion::parse("1.2.3-rc.10");
        require(stable && rc && rc10 && stable->compare(*rc) > 0 && rc10->compare(*rc) > 0,
                "SemVer precedence incorrect");
        UpdateSource source;
        source.private_repository_requires_authentication = true;
        auto auth = SoftwareUpdateService(source, "1.2.3").check(UpdateChannel::github);
        require(auth.status == UpdateStatus::needs_authorization,
                "Private update checks must request session authorization");
        require(SoftwareUpdateService(source, "1.2.3").check(UpdateChannel::gitee).status ==
                    UpdateStatus::not_configured,
                "Unconfigured Gitee must not check");
        source.gitee_repository = "fleetingxhx/SonyConnect";
        source.gitee_release_page = "https://gitee.com/fleetingxhx/SonyConnect/releases";
        source.gitee_manifest = "https://gitee.com.evil.test/fleetingxhx/SonyConnect/raw/main/update.json";
        require(SoftwareUpdateService(source, "1.2.3").check(UpdateChannel::gitee).status ==
                    UpdateStatus::failed,
                "Untrusted Gitee host accepted");
        std::atomic_bool canceled = true;
        require(SoftwareUpdateService(source, "1.2.3").check(UpdateChannel::github, L"", &canceled).status ==
                    UpdateStatus::canceled,
                "Cancellation ignored");
        require(DiagnosticLog::redact("00:11:22:33:44:55 001122334455") == "[redacted] [redacted]",
                "Bluetooth identifiers not redacted");
        require(DiagnosticLog::redact("id_001122334455_suffix") == "id_[redacted]_suffix",
                "Embedded device identifier not redacted");
        require(DiagnosticLog::redact("123456789abcd") == "123456789abcd",
                "Longer hexadecimal value wrongly redacted");
        write(temp / L"legacy.scdiag", legacy_fixture);
        auto imported = DiagnosticLog::read_replay(temp / L"legacy.scdiag");
        require(imported.size() == 1 && imported[0].entry.event == "connected",
                "Legacy compressed diagnostic import failed");
        require(imported[0].entry.status == "address=[redacted]" && imported[0].entry.model == "WH-1000XM5",
                "Legacy diagnostic values not sanitized");
        auto corrupt = legacy_fixture;
        corrupt[43] ^= 1;
        write(temp / L"corrupt.scdiag", corrupt);
        rejects([&] { DiagnosticLog::read_replay(temp / L"corrupt.scdiag"); },
                "Corrupt diagnostic data accepted");
        auto excessive = legacy_fixture;
        for (std::size_t i = 304; i < 308; ++i)
            excessive[i] = static_cast<char>(0xff);
        write(temp / L"excessive.scdiag", excessive);
        rejects([&] { DiagnosticLog::read_replay(temp / L"excessive.scdiag"); },
                "Unbounded ZIP size accepted");
        {
            DiagnosticLog log(temp / L"logs");
            DiagnosticEvent e;
            e.event = "confirmed";
            e.model = "WH-1000XM5";
            e.status = "address=00:11:22:33:44:55";
            log.record(e);
        }
        {
            DiagnosticLog export_log(temp / L"logs");
            DiagnosticEvent latest;
            latest.event = "just-queued";
            export_log.record(latest);
            export_log.export_archive(temp / L"native.scdiag");
            auto source_log = std::filesystem::directory_iterator(temp / L"logs")->path();
            auto before = read(source_log);
            rejects([&] { export_log.export_archive(source_log); }, "Source log overwritten by export");
            require(read(source_log) == before, "Export changed a source log");
        }
        auto native = DiagnosticLog::read_replay(temp / L"native.scdiag");
        require(std::any_of(native.begin(), native.end(),
                            [](const auto &e) {
                                return e.entry.event == "confirmed" && e.entry.status == "address=[redacted]";
                            }),
                "Native export lost recorded event");
        require(std::any_of(native.begin(), native.end(),
                            [](const auto &e) { return e.entry.event == "just-queued"; }),
                "Export did not drain pending diagnostic events");
        require(read(temp / L"native.scdiag").find("00:11:22:33:44:55") == std::string::npos,
                "Archive leaked Bluetooth address");
        rejects([&] { DiagnosticOptions{0, 10}.validate(); }, "Invalid diagnostic limit accepted");
        // Only the exclusively created test directory is removed.
        std::filesystem::remove_all(temp);
        winrt::uninit_apartment();
        std::cout << "Desktop tests passed\n";
        return 0;
    } catch (const winrt::hresult_error &error) {
        std::cerr << "Desktop tests failed (HRESULT " << std::hex << error.code().value
                  << "): " << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "Desktop tests failed: " << error.what() << '\n';
        return 1;
    }
}
