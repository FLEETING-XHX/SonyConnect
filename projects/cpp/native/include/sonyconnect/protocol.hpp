#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Independent port of SonyConnect.Core.Protocol. Wire definitions: pinned MIT
// SonyHeadphonesClient 193c1e053836119eb875c97331dd10c1aef206bc. No APK/AGPL code.
namespace sonyconnect::protocol {
using Byte = std::uint8_t;
using Bytes = std::vector<Byte>;
inline constexpr Byte ack_type = 0x01, table1_type = 0x0c, table2_type = 0x0e;
inline constexpr std::size_t maximum_payload = 64 * 1024; // Local memory policy.

struct Frame {
    Byte type{}, sequence{};
    Bytes payload;
    bool requires_acknowledgement() const { return type == table1_type || type == table2_type; }
    bool operator==(const Frame &) const = default;
};
enum class FrameError {
    Incomplete,
    BadMarker,
    InvalidEscape,
    BodyTooShort,
    PayloadTooLarge,
    LengthMismatch,
    BadChecksum,
    InterruptedFrame
};
struct FrameResult {
    std::optional<Frame> frame;
    std::optional<FrameError> error;
    bool success() const { return frame.has_value() && !error; }
};
Bytes pack(const Frame &, std::size_t limit = maximum_payload);
FrameResult unpack(std::span<const Byte>, std::size_t limit = maximum_payload);
class StreamDecoder {
  public:
    explicit StreamDecoder(std::size_t limit = maximum_payload);
    std::vector<FrameResult> feed(std::span<const Byte>);
    std::optional<FrameResult> complete();
    void reset();
    std::size_t buffered_bytes() const { return body_.size(); }

  private:
    std::size_t limit_;
    Bytes body_;
    bool started_{}, escaped_{};
    std::optional<std::size_t> expected_;
};

enum class Family { Unknown, V1, V2 };
enum class Table { Table1, Table2 };
enum class Kind { Acknowledgement, Response, Notification, Query, Unknown };
enum class ParseError {
    InvalidAcknowledgement,
    InvalidSequence,
    EmptyPayload,
    MalformedPayload,
    ProtocolNotNegotiated
};
enum class DeviceInfoField : Byte { ModelName = 1, Firmware = 2 };
enum class SoundMode { NoiseCanceling, Ambient, Off };
enum class BatteryLayout { Unknown, LeftRightCase, Single };
enum class Feature {
    Firmware,
    LeftBattery,
    RightBattery,
    CaseBattery,
    SingleBattery,
    SoundMode,
    AmbientLevel,
    VoiceFocus,
    Equalizer,
    ClearBass,
    Multipoint,
    ConnectedSources,
    SourceSwitch,
    FixedPlayback,
    ConnectionPreference,
    NegotiatedCodec,
    Dsee,
    SpeakToChat,
    WearDetectionPause,
    AutoPowerOff,
    TouchAssignment,
    AmbientCycle,
    Notifications,
    PromptVolume,
    PowerOff,
    SoundPressure
};
enum class Support { Unknown, Unsupported, Supported };
struct FeatureAccess {
    Support read{Support::Unknown}, write{Support::Unknown}, notify{Support::Unknown};
};
struct Capability {
    FeatureAccess access;
    std::string reason;
    bool hardware_verified{false};
};
struct Capabilities {
    std::map<Feature, Capability> features;
    bool complete{};
    bool readable(Feature feature) const;
};

struct Query {
    Byte type{};
    Bytes payload;
    Byte response_command{}, inquired_type{};
    bool await_response{true};
    Frame to_frame(Byte sequence) const;
    bool operator==(const Query &) const = default;
};
Query read_query(Byte table, Byte command, Byte subtype);
Query write_query(Byte table, Bytes payload);
Query protocol_info();
Query capability_info();
Query model_name();
Query firmware();
Query functions(Table);
Frame acknowledgement_for(const Frame &);

struct ProtocolInformation {
    Family family{};
    std::uint32_t wire_version{};
    bool table1{};
    std::optional<bool> table2;
};
struct CapabilityInformation {
    Byte counter{};
    Bytes identifier;
};
struct DeviceInformation {
    DeviceInfoField field{};
    std::string value;
};
struct SupportFunction {
    Byte id{}, priority{};
    bool operator==(const SupportFunction &) const = default;
};
struct SupportFunctions {
    Table table{};
    std::vector<SupportFunction> functions;
    bool contains(Byte id) const;
};
struct BatteryUnit {
    Byte level{}, charging{};
};
struct BatteryStatus {
    Byte inquired_type{};
    BatteryUnit first;
    std::optional<BatteryUnit> second;
};
struct AmbientRange {
    int minimum{}, maximum{}, step{};
    bool contains(int value) const;
};
struct SoundCapability {
    std::map<Byte, AmbientRange> ranges;
};
struct SoundAvailability {
    bool available{};
};
struct SoundParameter {
    bool settled{}, enabled{};
    Byte mode{}, ambient_mode{}, level{};
    SoundMode sound_mode() const;
    Query set(std::optional<SoundMode> mode = {}, std::optional<int> level = {},
              std::optional<bool> voice_focus = {}) const;
};
struct EqualizerPreset {
    Byte id{};
    std::string name;
};
struct EqualizerCapability {
    Byte band_count{}, step_count{};
    std::vector<EqualizerPreset> presets;
};
struct EqualizerAvailability {
    bool available{};
};
struct EqualizerParameter {
    Byte preset_id{};
    Bytes steps;
    bool five_bands() const;
    std::vector<int> bands() const;
};
enum class ExtendedField { Capability, Availability, Parameter, Codec };
struct ExtendedValue {
    Feature feature{};
    Byte group{}, type{};
    ExtendedField field{};
    Bytes data;
};
struct ExtendedProfile {
    Feature feature{};
    Byte function{}, group{}, type{};
    Byte status_type() const;
    bool operator==(const ExtendedProfile &) const = default;
};
struct GeneralSettingCapability {
    Byte slot{};
    bool boolean{};
    Byte string_format{};
    std::string name;
    bool multipoint() const;
};
struct GeneralSettingAvailability {
    Byte slot{};
    bool available{};
};
struct GeneralSettingParameter {
    Byte slot{};
    bool enabled{};
};
struct SourceCapability {
    Byte subtype{}, maximum_paired{}, maximum_connected{};
};
struct SourceAvailability {
    Byte subtype{};
    bool available{};
};
struct MultipointAlert {
    Byte message{}, action{};
};
struct SourceEntry {
    std::string id, name;
    Byte connection_slot{};
    std::optional<int> class_of_device;
};
struct SourceList {
    Byte subtype{};
    std::vector<SourceEntry> entries;
    Byte playback_slot{};
    std::optional<std::string> active_source() const;
};
struct SourceSwitchResult {
    Byte result{};
    std::string target;
};
enum class SourceCategory { Unknown, Computer, Phone, Other };
struct TouchAction {
    Byte action{}, function{};
    bool operator==(const TouchAction &) const = default;
};
struct TouchCustomAction {
    Byte action{}, default_function{};
    Bytes functions;
};
struct TouchPreset {
    Byte id{};
    Bytes functions;
    std::vector<TouchAction> actions;
    std::vector<TouchCustomAction> customizable;
};
struct TouchPresetState {
    Byte preset{};
    std::vector<TouchAction> actions;
};
struct TouchKey {
    Byte id{}, type{}, default_preset{};
    std::vector<TouchPreset> presets;
};
struct TouchCapability {
    Byte type{}, limitation{};
    std::vector<TouchKey> keys;
};
struct TouchStatus {
    Byte type{};
    Bytes values;
};
struct TouchSelection {
    Byte type{};
    Bytes values;
};
struct TouchDetails {
    Byte type{};
    std::vector<TouchPresetState> values;
};
struct CycleValue {
    Byte command{}, value{};
};
struct GuidanceValue {
    Byte command{}, type{};
    Bytes data;
};
struct FixValue {
    bool fixed{};
    std::optional<Byte> result;
};
struct VoiceAssistantCapability {
    Byte key_type{};
    Bytes candidates;
};
struct VoiceAssistantValue {
    Byte value{};
};
using Information =
    std::variant<std::monostate, ProtocolInformation, CapabilityInformation, DeviceInformation,
                 SupportFunctions, BatteryStatus, SoundCapability, SoundAvailability, SoundParameter,
                 EqualizerCapability, EqualizerAvailability, EqualizerParameter, ExtendedValue,
                 GeneralSettingCapability, GeneralSettingAvailability, GeneralSettingParameter,
                 SourceCapability, SourceAvailability, MultipointAlert, SourceList, SourceSwitchResult,
                 TouchCapability, TouchStatus, TouchSelection, TouchDetails, CycleValue, GuidanceValue,
                 FixValue, VoiceAssistantCapability, VoiceAssistantValue>;
struct Message {
    Frame frame;
    Kind kind{Kind::Unknown};
    std::optional<Byte> command;
    Information information;
};
struct ParseResult {
    Message message;
    std::optional<ParseError> error;
    bool valid() const { return !error; }
};
ParseResult parse(const Frame &, Family = Family::Unknown);

enum class Phase {
    Protocol,
    CapabilityInfo,
    ModelName,
    Firmware,
    Table1Functions,
    Table2Functions,
    Ready,
    Unsupported
};
struct HandshakeState {
    std::uint64_t session{};
    Phase phase{Phase::Protocol};
    std::optional<ProtocolInformation> protocol;
    std::optional<CapabilityInformation> capability;
    std::optional<std::string> model_name, firmware;
    std::optional<SupportFunctions> table1_functions, table2_functions;
    std::string error;
};
struct HandshakeStep {
    bool applied{};
    std::optional<ParseResult> parsed;
    std::string diagnostic;
};
class Handshake {
  public:
    explicit Handshake(std::uint64_t session);
    const HandshakeState &state() const { return state_; }
    std::optional<Query> current_query() const;
    HandshakeStep receive(std::uint64_t session, const Frame &);

  private:
    HandshakeState state_;
};
Capabilities map_capabilities(const HandshakeState &);

struct BatteryParameters {
    static std::vector<Query> queries(const SupportFunctions *, BatteryLayout);
};
struct SoundParameters {
    static Query capability();
    static Query availability();
    static Query read();
};
struct EqualizerParameters {
    static Query capability();
    static Query availability();
    static Query read();
    static Query set(Byte preset, const Bytes &steps);
    static std::string preset_name(Byte);
};
struct ExtendedParameters {
    static const std::vector<ExtendedProfile> &profiles();
    static Query capability(const ExtendedProfile &);
    static Query availability(const ExtendedProfile &);
    static Query read(const ExtendedProfile &);
    static std::vector<Query> read_queries(const ExtendedProfile &);
    static Query codec();
    static Query confirm_connection_change();
    static bool legal(const ExtendedProfile &, Byte value);
    static Query write(const ExtendedProfile &, Byte value, Byte last_selected = 0);
};
struct AudioSourceParameters {
    static Query enable_alerts();
    static Query confirm_multipoint_alert(Byte message);
    static Query general_capability(Byte slot);
    static Query general_availability(Byte slot);
    static Query general_read(Byte slot);
    static Query set_multipoint(Byte slot, bool enabled);
    static Query source_capability(Byte subtype);
    static Query source_availability(Byte subtype);
    static Query sources(Byte subtype);
    static Query switch_source(const std::string &);
    static SourceCategory category(std::optional<int> class_of_device);
};
struct InteractionParameters {
    static bool known_voice_assistant(Byte);
    static bool known_preset(Byte);
    static bool preset_allowed(Byte, Byte limitation);
    static Query voice_assistant_read();
    static Query voice_assistant_write(Byte);
    static Query touch_read(Byte);
    static Query touch_write(Byte, const Bytes &);
    static Query touch_details_read(Byte);
    static Query touch_details_write(Byte, const std::vector<TouchPresetState> &);
    static Query reply_touch_voice_assistant_alert(bool accept);
    static Query reply_restore_voice_assistant_alert();
    static Query cycle_read();
    static Query cycle_write(Byte);
    static Query guidance_status(Byte);
    static Query guidance_write(Byte, Byte, std::optional<Byte> language);
    static Query volume_read();
    static Query volume_write(int);
    static Query fix_read();
    static Query fix_write(bool);
};
} // namespace sonyconnect::protocol
