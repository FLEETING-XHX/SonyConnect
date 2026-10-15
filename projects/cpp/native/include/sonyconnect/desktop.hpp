#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sonyconnect::desktop {

enum class SoundMode { noise_canceling = 0, ambient = 1, off = 2 };
enum class Recognizer : std::uint32_t {
    none = 0,
    left_ctrl = 1,
    right_ctrl = 2,
    left_alt = 4,
    right_alt = 8,
    left_shift = 16,
    right_shift = 32,
    caps_lock = 64,
    tab = 128
};
struct HotkeyGesture {
    Recognizer recognizer{Recognizer::none};
    int key{};
    bool operator==(const HotkeyGesture &) const = default;
    std::wstring label() const;
};
struct HotkeyBinding {
    SoundMode mode{};
    HotkeyGesture gesture;
};
using HotkeyBindings = std::array<HotkeyBinding, 3>;
Recognizer recognizer_for_key(int key);
bool is_free_key(int key);
HotkeyBindings default_hotkeys();
void validate_hotkeys(const HotkeyBindings &bindings);
struct HotkeyCaptureResult {
    std::optional<HotkeyGesture> gesture;
    std::wstring preview, message;
    bool finished{};
};
class HotkeyCapture {
  public:
    HotkeyCaptureResult begin();
    void end();
    bool active() const { return active_; }
    HotkeyCaptureResult feed(int key, bool down, bool repeat = false);

  private:
    HotkeyCaptureResult progress() const;
    std::set<int> held_;
    std::uint32_t recognizers_{};
    bool active_{};
};
struct HotkeyMatch {
    std::optional<SoundMode> mode;
    bool consume{};
};
class HotkeyMatcher {
  public:
    HotkeyMatch feed(int key, bool down, bool injected, bool paused, const HotkeyBindings &bindings);
    void reset();

  private:
    std::set<int> held_, consumed_;
    std::uint32_t recognizers_{};
};
class HotkeyStore {
  public:
    explicit HotkeyStore(std::filesystem::path path);
    HotkeyBindings load();
    void save(const HotkeyBindings &bindings);
    const std::wstring &warning() const { return warning_; }

  private:
    std::filesystem::path path_;
    bool future_version_{};
    std::wstring warning_;
};
namespace detail {
// Replaces only platform side effects, so offline tests never register a real hotkey or hook.
struct HotkeyPlatform {
    std::function<void(const HotkeyBindings &)> check_conflicts;
    std::function<void()> start_listener, stop_listener;
};
} // namespace detail
// Callback runs on the hook thread: post to the UI and return immediately.
// Only held key state is retained; no text or keyboard history is recorded.
class GlobalHotkeys {
  public:
    GlobalHotkeys(std::filesystem::path store_path, std::function<void(SoundMode)> dispatch);
    GlobalHotkeys(std::filesystem::path store_path, std::function<void(SoundMode)> dispatch,
                  detail::HotkeyPlatform platform);
    ~GlobalHotkeys();
    GlobalHotkeys(const GlobalHotkeys &) = delete;
    GlobalHotkeys &operator=(const GlobalHotkeys &) = delete;
    void start();
    void save(const HotkeyBindings &bindings);
    void pause(bool value);
    HotkeyBindings bindings() const;
    std::wstring status() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct VolumeState {
    std::wstring endpoint_id, endpoint_name, message;
    double percent{};
    bool muted{}, can_write{};
};
enum class MediaAction { play, pause, previous, next };
struct MediaState {
    std::wstring session_id, title, application_id, message;
    bool playing{}, can_play{}, can_pause{}, can_previous{}, can_next{};
    bool allows(MediaAction action) const;
};
struct LocalPlaybackSnapshot {
    VolumeState volume;
    MediaState media;
};
struct DesktopResult {
    bool success{};
    std::wstring message;
};
// Resolves Windows' actual Bluetooth AssociationEndpoint id; an empty result means unresolved.
std::wstring bluetooth_aep_id(std::uint64_t bluetooth_address);
// Read-only, on a WinRT initialized MTA worker. No access prompt or state change.
// true: at least one Bluetooth radio is On; false: no radios or all are Off/Disabled;
// nullopt: API failure or an indeterminate radio state, never treated as Off.
std::optional<bool> bluetooth_available();
// Call on a COM initialized worker (MTA), never synchronously on the UI thread.
// selected_aep_id must be the Bluetooth AssociationEndpoint DeviceInformation id.
// Passing no selection safely disables endpoint volume writes.
class LocalPlaybackBackend {
  public:
    LocalPlaybackBackend();
    ~LocalPlaybackBackend();
    LocalPlaybackBackend(const LocalPlaybackBackend &) = delete;
    LocalPlaybackSnapshot read(const std::wstring &selected_aep_id = {});
    DesktopResult set_volume(const std::wstring &selected_aep_id, const std::wstring &expected_endpoint,
                             double percent);
    DesktopResult execute_media(const std::wstring &expected_session, MediaAction action);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class UpdateChannel { github, gitee };
enum class UpdateStatus {
    not_configured,
    needs_authorization,
    available,
    current,
    no_release,
    failed,
    canceled
};
struct ReleaseVersion {
    int major{}, minor{}, patch{};
    std::optional<std::string> prerelease;
    static std::optional<ReleaseVersion> parse(const std::string &text);
    int compare(const ReleaseVersion &other) const;
};
struct UpdateSource {
    std::string repository{"FLEETING-XHX/SonyConnect"};
    bool private_repository_requires_authentication{true};
    std::string gitee_repository, gitee_release_page, gitee_manifest;
    static UpdateSource load(const std::filesystem::path &path);
    void validate() const;
    std::wstring release_page(UpdateChannel channel) const;
};
struct UpdateResult {
    UpdateStatus status{UpdateStatus::failed};
    std::wstring message, latest;
};
// A network request occurs only when check() is explicitly called by a user action.
// session_token is never persisted or logged; redirects are disabled.
class SoftwareUpdateService {
  public:
    SoftwareUpdateService(UpdateSource source, std::string current_version);
    UpdateResult check(UpdateChannel channel, const std::wstring &session_token = {},
                       const std::atomic_bool *canceled = nullptr) const;

  private:
    UpdateSource source_;
    std::string current_;
};

struct DiagnosticOptions {
    unsigned segment_limit_mib{2}, retained_runs{10};
    void validate() const;
};
struct DiagnosticEvent {
    std::string event, status, phase, error_code, model, firmware, parameter, origin;
    std::string selection_id, link_id, request_id;
    std::optional<int> type, sequence, command, payload_bytes, requested_value, confirmed_value;
    std::optional<std::int64_t> generation, receive_order;
};
struct DiagnosticReplayEntry {
    std::string timestamp;
    DiagnosticEvent entry;
};
// Four rotating JSONL segments. Exports use the existing .scdiag ZIP schema.
// Imports are bounded, read-only timelines and never dispatch protocol commands.
class DiagnosticLog {
  public:
    explicit DiagnosticLog(std::filesystem::path directory, DiagnosticOptions options = {});
    ~DiagnosticLog();
    DiagnosticLog(const DiagnosticLog &) = delete;
    void record(DiagnosticEvent entry);
    void apply_options(DiagnosticOptions options);
    void export_archive(const std::filesystem::path &destination);
    std::wstring status() const;
    static std::vector<DiagnosticReplayEntry> read_replay(const std::filesystem::path &archive);
    static std::string redact(std::string value, std::size_t limit = 512);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sonyconnect::desktop
