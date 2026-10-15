#pragma once

#include "sonyconnect/protocol.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sonyconnect {
using Milliseconds = std::chrono::milliseconds;
enum class ResultCode {
    Success,
    Delivered,
    Busy,
    NotConnected,
    Unavailable,
    Unsupported,
    InvalidValue,
    Timeout,
    Transport,
    Protocol,
    DeviceRejected,
    Canceled,
    Superseded,
    ConfirmationRequired
};
struct Result {
    ResultCode code{ResultCode::Success};
    std::string message;
    bool hardware_verified{false};
    bool ok() const { return code == ResultCode::Success || code == ResultCode::Delivered; }
    bool confirmed() const { return code == ResultCode::Success; }
};
struct DeviceDescriptor {
    std::uint64_t address{};
    std::string name, identity;
    bool paired{}, connected_hint{}, sony_service_hint{};
    std::wstring aep_id; // Resolved with desktop::bluetooth_aep_id on the MTA UI worker.
};
struct DiscoveryResult {
    Result result;
    std::vector<DeviceDescriptor> devices;
};
// Discovery includes paired Sony candidates with an active Windows Bluetooth link only.
bool is_connected_sony_device(const DeviceDescriptor &);
DiscoveryResult discover_paired_devices();
enum class ReadStatus { Data, Timeout, Closed };
struct ReadResult {
    ReadStatus status{ReadStatus::Timeout};
    std::size_t count{};
};

// A read timeout never means EOF. Close must interrupt pending native operations.
class ByteTransport {
  public:
    virtual ~ByteTransport() = default;
    virtual Result open(const DeviceDescriptor &, Milliseconds timeout) = 0;
    virtual ReadResult read(std::span<protocol::Byte>, Milliseconds timeout) = 0;
    virtual void write(std::span<const protocol::Byte>, Milliseconds timeout) = 0;
    virtual void close() noexcept = 0;
};
std::unique_ptr<ByteTransport> make_windows_rfcomm_transport();
struct SessionPolicy {
    Milliseconds connect_timeout{8000}, request_timeout{5000}, write_timeout{3000}, retry_delay{300};
    unsigned maximum_connect_attempts{1}, maximum_query_attempts{1};
};
struct Reply {
    Result result;
    std::optional<protocol::Frame> response;
    std::uint64_t receive_order{};
};
struct Observation {
    protocol::Message message;
    std::uint64_t link{}, receive_order{};
};
struct ProtocolTrace {
    std::string event;
    std::uint64_t link{}, receive_order{};
    std::optional<protocol::Byte> type, sequence, command;
};
class ProtocolSession {
  public:
    explicit ProtocolSession(std::unique_ptr<ByteTransport>, SessionPolicy = {});
    ~ProtocolSession();
    ProtocolSession(const ProtocolSession &) = delete;
    ProtocolSession &operator=(const ProtocolSession &) = delete;
    Result connect(const DeviceDescriptor &);
    void disconnect() noexcept;
    void abort() noexcept; // Immediate socket close; thread joining is deferred.
    Reply exchange(const protocol::Query &);
    bool connected() const;
    std::uint64_t link() const;
    void set_observer(std::function<void(const Observation &)>);
    void set_trace(std::function<void(const ProtocolTrace &)>);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class ValueStatus { Unknown, NotRead, Known, Unsupported, Stale };
template <typename T> struct Value {
    ValueStatus status{ValueStatus::Unknown};
    std::optional<T> value;
    std::uint64_t receive_order{};
    bool notification_observed{};
    std::string reason;
    bool known() const { return status == ValueStatus::Known && value.has_value(); }
};
struct FeatureState {
    Value<int> value;
    std::vector<int> choices;
    bool advertised{}, available{}, writable{};
    std::string reason;
};
struct BatterySnapshot {
    Value<int> single, left, right, case_level;
    Value<bool> single_charging, left_charging, right_charging, case_charging;
};
struct SoundSnapshot {
    Value<protocol::SoundMode> mode;
    Value<int> ambient_level;
    Value<bool> voice_focus;
    std::map<protocol::Byte, protocol::AmbientRange> ranges;
    bool available{}, writable{};
};
struct EqualizerSnapshot {
    Value<int> preset;
    Value<std::vector<int>> bands;
    Value<int> clear_bass;
    std::vector<protocol::EqualizerPreset> presets;
    std::optional<int> band_count, step_count;
    bool available{}, five_band_layout{}, writable{};
    std::string reason;
};
struct SourceSnapshot {
    std::vector<protocol::SourceEntry> entries;
    Value<std::string> active_source;
    Value<bool> multipoint;
    bool available{}, switch_writable{}, multipoint_writable{};
};
struct TouchSnapshot {
    std::vector<protocol::TouchKey> keys;
    Value<protocol::Bytes> assignments;
    Value<std::vector<protocol::TouchPresetState>> details;
    Value<int> cycle, guidance, prompt_volume, voice_assistant;
    Value<bool> playback_fixed;
    protocol::Bytes voice_assistant_candidates;
    std::optional<protocol::Bytes> linked_backup_assignments;
    std::optional<int> linked_backup_assistant;
    bool writable{}, cycle_writable{}, guidance_writable{}, volume_writable{}, fixation_writable{},
        assistant_writable{};
};
struct DeviceSnapshot {
    bool connected{}, ready{}, busy{}, hardware_verified{false};
    std::uint64_t link{}, revision{};
    std::optional<DeviceDescriptor> device;
    std::string model, firmware, status;
    protocol::Family family{protocol::Family::Unknown};
    protocol::Capabilities capabilities;
    std::map<protocol::Feature, FeatureState> features;
    BatterySnapshot battery;
    SoundSnapshot sound;
    EqualizerSnapshot equalizer;
    SourceSnapshot sources;
    TouchSnapshot touch;
    Value<std::string> codec;
    std::vector<protocol::MultipointAlert> pending_alerts;
    Result last_result;
};

// Synchronous commands belong on the UI worker. Notifications are applied on a
// dedicated receive thread. snapshot() returns a coherent copy for rendering.
class DeviceController {
  public:
    explicit DeviceController(SessionPolicy = {});
    DeviceController(std::unique_ptr<ByteTransport>, SessionPolicy = {});
    ~DeviceController();
    DiscoveryResult discover() const;
    Result connect(const DeviceDescriptor &);
    void disconnect() noexcept;
    void suspend() noexcept;
    Result resume();
    void notify_bluetooth_available(bool available) noexcept;
    Result refresh();
    DeviceSnapshot snapshot() const;
    Result set(protocol::Feature, int);
    Result set_sound(std::optional<protocol::SoundMode> mode = {}, std::optional<int> level = {},
                     std::optional<bool> voice_focus = {});
    Result set_equalizer(std::optional<protocol::Byte> preset = {}, std::optional<int> band = {},
                         std::optional<int> gain = {}, std::optional<int> bass = {});
    Result switch_source(const std::string &address);
    Result set_touch(protocol::Byte key, protocol::Byte preset, bool confirm_linked_change = false);
    Result restore_touch_linked_backup();
    Result set_touch_details(const std::vector<protocol::TouchPresetState> &);
    Result set_voice_assistant(protocol::Byte);
    void set_trace(std::function<void(const ProtocolTrace &)>);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sonyconnect
