#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sonyconnect::desktop {

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
std::wstring bluetooth_aep_id(std::uint64_t bluetooth_address, const std::atomic_bool *closing = nullptr);
// Read-only, on a WinRT initialized MTA worker. No access prompt or state change.
// true: at least one Bluetooth radio is On; false: no radios or all are Off/Disabled;
// nullopt: API failure or an indeterminate radio state, never treated as Off.
std::optional<bool> bluetooth_available(const std::atomic_bool *closing = nullptr);
// Call on a COM initialized worker (MTA), never synchronously on the UI thread.
// selected_aep_id must be the Bluetooth AssociationEndpoint DeviceInformation id.
// Passing no selection safely disables endpoint volume writes.
class LocalPlaybackBackend {
  public:
    // A supplied cancellation flag must outlive this backend and its calls.
    explicit LocalPlaybackBackend(const std::atomic_bool *closing = nullptr);
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
    std::string gitee_repository, gitee_release_page, gitee_manifest, gitee_api;
    static UpdateSource load(const std::filesystem::path &path);
    void validate() const;
    std::wstring release_page(UpdateChannel channel) const;
};
struct UpdateResult {
    UpdateStatus status{UpdateStatus::failed};
    std::wstring message, latest;
};
struct UpdateHttpResponse { unsigned status{}; std::string body; };
// A network request occurs only when check() is explicitly called by a user action.
// session_token is never persisted or logged; redirects are disabled.
class SoftwareUpdateService {
  public:
    using Fetch = std::function<UpdateHttpResponse(const std::wstring &, const std::wstring &,
                                                   const std::atomic_bool *)>;
    SoftwareUpdateService(UpdateSource source, std::string current_version, Fetch fetch = {});
    UpdateResult check(UpdateChannel channel, const std::wstring &session_token = {},
                       const std::atomic_bool *canceled = nullptr) const;

  private:
    UpdateSource source_;
    std::string current_;
    Fetch fetch_;
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
