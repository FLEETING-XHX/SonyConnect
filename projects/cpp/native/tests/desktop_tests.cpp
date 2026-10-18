#include "sonyconnect/desktop.hpp"
#include "sonyconnect/desktop_async.hpp"
#include "sonyconnect/tray_icon.hpp"
#include <winrt/base.h>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <functional>
#include <thread>

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
struct PendingAsync {
    struct State {
        std::function<void()> complete;
        unsigned cancellations{}, results{};
        HRESULT error{S_OK};
        bool immediate{};
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    template <class Handler> void Completed(Handler handler) const {
        state->complete = [handler] { handler(0, winrt::Windows::Foundation::AsyncStatus::Completed); };
        if (state->immediate) state->complete();
    }
    void Cancel() const { ++state->cancellations; } // Provider deliberately ignores cancellation.
    int GetResults() const {
        ++state->results;
        winrt::check_hresult(state->error);
        return 42;
    }
};
void async_checks() {
    using namespace std::chrono;
    PendingAsync pending;
    const auto begin = steady_clock::now();
    HRESULT failure{};
    try { detail::await_async([&] { return pending; }, nullptr, milliseconds(40)); }
    catch (const winrt::hresult_error &e) { failure = e.code(); }
    require(failure == HRESULT_FROM_WIN32(ERROR_TIMEOUT) && pending.state->cancellations == 1 &&
                pending.state->results == 0 && steady_clock::now() - begin < seconds(1),
            "Ignored provider cancellation must not keep the worker waiting");
    pending.state->complete(); // Late completion after the wait frame was destroyed.
    PendingAsync immediate;
    immediate.state->immediate = true;
    require(detail::await_async([&] { return immediate; }) == 42,
            "An operation after a timeout must still run");
    immediate.state->error = E_ACCESSDENIED;
    failure = S_OK;
    try { detail::await_async([&] { return immediate; }); }
    catch (const winrt::hresult_error &e) { failure = e.code(); }
    require(failure == E_ACCESSDENIED, "Provider failures must be propagated");
    std::atomic_bool closing{true};
    bool started = false;
    rejects([&] { detail::await_async([&] { started = true; return pending; }, &closing); },
            "A closing host must reject new asynchronous work");
    require(!started, "Cancellation must be checked before invoking the provider");
    closing = false;
    const auto cancel_begin = steady_clock::now();
    std::jthread cancel([&] { std::this_thread::sleep_for(milliseconds(10)); closing = true; });
    failure = S_OK;
    try { detail::await_async([&] { return pending; }, &closing); }
    catch (const winrt::hresult_error &e) { failure = e.code(); }
    require(failure == HRESULT_FROM_WIN32(ERROR_CANCELLED) &&
                steady_clock::now() - cancel_begin < seconds(1), "Exit must interrupt the async wait");
    pending.state->complete();
}
void tray_checks() {
    unsigned added = 0, removed = 0;
    bool available = true;
    {
        TrayIcon icon(nullptr, WM_APP + 2, nullptr, [&](DWORD command, NOTIFYICONDATAW *data) {
            require(data->uCallbackMessage == WM_APP + 2 && data->uID == 1, "Tray identity must remain stable");
            if (command == NIM_DELETE) { ++removed; return TRUE; }
            ++added; return available ? TRUE : FALSE;
        });
        require(icon.restore() && icon.added(), "Initial tray add failed");
        require(icon.taskbar_created(RegisterWindowMessageW(L"TaskbarCreated")), "Restart message is not registered");
        require(icon.restore() && added == 2, "Taskbar restart must re-add the icon");
        available = false;
        require(!icon.restore() && !icon.added(), "Failed restoration must clear the added flag");
        available = true;
        require(icon.restore(), "Tray must recover after a failed add");
    }
    require(added == 4 && removed == 1, "Tray lifecycle dispatch mismatch");
}
void retention_checks(const std::filesystem::path &temp) {
    const auto root = temp / L"retention";
    DiagnosticLog first(root, {1, 1});
    auto first_file = std::filesystem::directory_iterator(root)->path();
    for (const auto &entry : std::filesystem::directory_iterator(root))
        if (entry.path().extension() == L".jsonl") first_file = entry.path();
    {
        DiagnosticLog second(root, {1, 1});
        require(std::filesystem::exists(first_file), "Retention must preserve another live writer");
    }
    const std::string stale = "run-20000101-000000000-00000000000000000000000000000001";
    const auto crashed = root / (stale + "-0.jsonl"), lease = root / (stale + ".lock");
    write(crashed, "unfinished\n");
    write(root / "user-note.jsonl", "unrelated\n");
    {
        winrt::file_handle live{CreateFileW(lease.c_str(), GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
        require(static_cast<bool>(live), "Cannot create fixture lease");
        first.apply_options({1, 1});
        require(std::filesystem::exists(crashed), "An open lease must protect unfinished logs");
    }
    first.apply_options({1, 1});
    require(!std::filesystem::exists(crashed) && !std::filesystem::exists(lease),
            "Abandoned logs without log-end must obey retention");
    require(std::filesystem::exists(first_file) && std::filesystem::exists(root / "user-note.jsonl"),
            "Cleanup must preserve live and unrelated files");
    // A pre-lease version's unfinished owned log has no running writer once
    // the product's single-instance guard admits this version.
    write(crashed, "legacy unfinished\n");
    first.apply_options({1, 1});
    require(!std::filesystem::exists(crashed), "Legacy abandoned logs must obey retention too");
}
std::string unhex(const std::string &hex) {
    std::string result;
    for (std::size_t i = 0; i < hex.size(); i += 2)
        result.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return result;
}
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
        async_checks();
        tray_checks();
        retention_checks(temp);
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
