#include "sonyconnect/protocol.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string_view>

using namespace sonyconnect::protocol;
namespace {
void check(bool condition, const char *message = "Protocol expectation failed") {
    if (!condition)
        throw std::runtime_error(message);
}
template <class Action> void throws(Action action) {
    bool caught = false;
    try {
        action();
    } catch (const std::exception &) {
        caught = true;
    }
    check(caught, "Invalid value was accepted");
}
Bytes hex(std::string_view text) {
    auto nibble = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c - 'A' + 10; };
    Bytes bytes;
    for (std::size_t i = 0; i < text.size(); i += 2)
        bytes.push_back(static_cast<Byte>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    return bytes;
}
Frame t1(Bytes payload, Byte sequence = 0) { return {table1_type, sequence, std::move(payload)}; }
Frame t2(Bytes payload, Byte sequence = 0) { return {table2_type, sequence, std::move(payload)}; }
ParseResult parsed(Bytes payload) { return parse(t1(std::move(payload)), Family::V2); }
template <class Value> Value value(const Frame &frame) {
    auto result = parse(frame, Family::V2);
    check(result.valid(), "Expected valid payload");
    return std::get<Value>(result.message.information);
}
template <class Value> Value value(Bytes payload) { return value<Value>(t1(std::move(payload))); }
void bad(Bytes payload) {
    check(parsed(std::move(payload)).error == ParseError::MalformedPayload, "Malformed data accepted");
}
void bad2(Bytes payload) {
    check(parse(t2(std::move(payload)), Family::V2).error == ParseError::MalformedPayload,
          "Malformed table 2 data accepted");
}
void append(Bytes &bytes, const Bytes &more) { bytes.insert(bytes.end(), more.begin(), more.end()); }

// Six byte-for-byte MIT reference captures, inherited from the C# fixture manifest:
// tests/SonyConnect.ProtocolTests/Fixtures/frames.json, pinned 193c1e0.
// These are upstream captures, not native application hardware validation.
const std::vector<std::string_view> fixtures{
    "3E0C01000000080100030030180000613C", "3E0C000000000D05010A57462D31303030584D358E3C",
    "3E0C0100000008050205362E312E30143C",
    "3E0C000000005F07002E10FF12FF13FF14FF15FF16FF17FF23FF253611FF27FF29FF2AFF92FF50116B08700190FFA112C1FFC2FF"
    "E21BF138FC0D93FFF633F941D13B94FF402E423A44FFFF2D19FFF326E71AD220FD2BB61DB042B243B81FB715343FF423F524293"
    "C",
    "3E0E010000001907000B423D2D53FF54FFF1FF60FF61FFF2FF63FFF830320431FFEE3C", "3E010100000000023C",
    // Independent synthetic fixtures from the same SonyConnect manifest.
    "3E0C000000000200000E3C", "3E010000000000013C", "3E0C0000000004FE3D2C3D2D3D2EC53C",
    "3E0C0000000001303D2D3C", "3E0C000000000803000104544553545C3C", "3E0C000000000425005000853C",
    "3E0C000000000B07000420FF50006B0523FF233C"};
Frame fixture(std::size_t index) {
    auto decoded = unpack(hex(fixtures.at(index)));
    check(decoded.success());
    return *decoded.frame;
}
Handshake bootstrap(bool table2 = true) {
    Handshake handshake(42);
    auto protocol = fixture(0);
    if (!table2)
        protocol.payload[7] = 1;
    for (const auto &frame : {protocol, fixture(10), fixture(1), fixture(2), fixture(3)})
        check(handshake.receive(42, frame).applied);
    if (table2)
        check(handshake.receive(42, fixture(4)).applied);
    return handshake;
}
Bytes invalid_declared_length(std::uint32_t length, Bytes payload) {
    Bytes body{table1_type,
               0,
               static_cast<Byte>(length >> 24),
               static_cast<Byte>(length >> 16),
               static_cast<Byte>(length >> 8),
               static_cast<Byte>(length)};
    append(body, payload);
    Byte sum = 0;
    for (auto b : body)
        sum = static_cast<Byte>(sum + b);
    body.push_back(sum);
    Bytes wire{0x3e};
    for (auto b : body) {
        if (b >= 0x3c && b <= 0x3e) {
            wire.push_back(0x3d);
            wire.push_back(static_cast<Byte>(b - 0x10));
        } else
            wire.push_back(b);
    }
    wire.push_back(0x3c);
    return wire;
}
const ExtendedProfile &profile(Feature feature, Byte type) {
    const auto &profiles = ExtendedParameters::profiles();
    auto found = std::find_if(profiles.begin(), profiles.end(),
                              [&](auto p) { return p.feature == feature && p.type == type; });
    check(found != profiles.end());
    return *found;
}
} // namespace
int main() {
    const std::vector<std::pair<const char *, std::function<void()>>> tests{
        {"safe-listening-wire-layout-and-table-isolation", [] {
             check(SafeListeningParameters::current(3).payload == Bytes({0x5a, 3}));
             check(SafeListeningParameters::set(3, true).payload == Bytes({0x58, 3, 0, 1}));
             check(SafeListeningParameters::set(0, false).payload == Bytes({0x58, 0, 1, 1}));
             const auto cap = value<SafeListeningCapability>(t2({0x51, 3, 1, 0x12, 0x34, 0x56, 0x78, 5, 9}));
             check(cap.timestamp_base == 0x12345678 && cap.minimum_interval == 5 && cap.log_capacity == 9);
             check(value<SafeListeningAvailability>(t2({0x57, 3, 0})).available);
             check(value<SafeListeningMode>(t2({0x59, 3, 0, 1})).enabled);
             check(!value<SafeListeningMode>(t2({0x59, 3, 0, 1})).preview);
             check(!std::holds_alternative<SafeListeningMode>(parsed({0x59, 3, 0, 1}).message.information));
             throws([] { SafeListeningParameters::current(4); });
             bad2({0x51, 3, 1});
             bad2({0x57, 3, 0, 1});
             bad2({0x59, 3, 2, 1});
             bad2({0x5b, 3, 0xff, 0xff});
             bad2({0x5b, 3, 72});
         }},
        {"sound-pressure-status-is-not-a-success-code", [] {
             // Synthetic semantic cases; these numbers are not hardware measurements.
             check(value<SoundPressure>(t2({0x5b, 3, 72, 0xff})).valid_level());
             check(value<SoundPressure>(t2({0x5b, 3, 72, 3})).valid_level());
             for (auto cause : {0, 1, 2})
                 check(!value<SoundPressure>(t2({0x5b, 3, 72, static_cast<Byte>(cause)})).valid_level());
             check(!value<SoundPressure>(t2({0x5b, 3, 0xff, 0})).valid_level());
             check(!SafeListeningParameters::select_type({Table::Table1, {{0x53, 0xff}}}));
             check(SafeListeningParameters::select_type({Table::Table2, {{0x53, 0xff}}}) == 3);
             check(!SafeListeningParameters::select_type({Table::Table2, {{0x51, 0}, {0x53, 1}}}));
         }},
        {"fixed-fixtures-roundtrip",
         [] {
             for (auto text : fixtures) {
                 auto bytes = hex(text);
                 auto decoded = unpack(bytes);
                 check(decoded.success());
                 check(pack(*decoded.frame) == bytes);
             }
         }},
        {"markers-in-payload-and-checksum-escaped",
         [] {
             check(pack(t1({0xfe, 0x3c, 0x3d, 0x3e})) == hex(fixtures[8]));
             check(pack(t1({0x30})) == hex(fixtures[9]));
         }},
        {"all-fixtures-every-split-and-single-byte",
         [] {
             for (auto text : fixtures) {
                 auto bytes = hex(text);
                 const auto expected = *unpack(bytes).frame;
                 for (std::size_t split = 0; split <= bytes.size(); ++split) {
                     StreamDecoder decoder;
                     auto output = decoder.feed(std::span<const Byte>(bytes).first(split));
                     auto rest = decoder.feed(std::span<const Byte>(bytes).subspan(split));
                     output.insert(output.end(), rest.begin(), rest.end());
                     check(output.size() == 1 && output[0].frame == expected);
                     check(!decoder.complete());
                 }
                 StreamDecoder decoder;
                 unsigned frames = 0;
                 for (auto byte : bytes)
                     for (const auto &event : decoder.feed(std::span<const Byte>(&byte, 1))) {
                         check(event.frame == expected);
                         ++frames;
                     }
                 check(frames == 1);
             }
         }},
        {"sticky-frames-ignore-noise",
         [] {
             Bytes bytes{0, 0xff, 0x3c};
             append(bytes, hex(fixtures[0]));
             append(bytes, hex(fixtures[5]));
             StreamDecoder decoder;
             auto events = decoder.feed(bytes);
             check(events.size() == 2 && events[0].frame == fixture(0) && events[1].frame == fixture(5));
         }},
        {"nested-start-recovers-next-frame",
         [] {
             Bytes bytes{0x3e, 0x0c};
             append(bytes, hex(fixtures[0]));
             StreamDecoder decoder;
             auto events = decoder.feed(bytes);
             check(events.size() == 2 && events[0].error == FrameError::InterruptedFrame &&
                   events[1].success());
         }},
        {"bad-checksum-recovers-following-frame",
         [] {
             auto bytes = hex(fixtures[0]);
             bytes[bytes.size() - 2] ^= 1;
             check(unpack(bytes).error == FrameError::BadChecksum);
             append(bytes, hex(fixtures[5]));
             StreamDecoder decoder;
             auto events = decoder.feed(bytes);
             check(events.size() == 2 && events[0].error == FrameError::BadChecksum &&
                   events[1].frame == fixture(5));
         }},
        {"escape-validation",
         [] {
             for (auto bytes : {hex("3E3D013C"), hex("3E3D3C"), hex("3E3D2F3C")})
                 check(unpack(bytes).error == FrameError::InvalidEscape);
             check(unpack(hex("3E3E3C")).error == FrameError::BadMarker);
         }},
        {"truncated-prefixes-report-eof",
         [] {
             for (auto text : fixtures) {
                 auto bytes = hex(text);
                 for (std::size_t n = 1; n < bytes.size(); ++n) {
                     StreamDecoder decoder;
                     check(decoder.feed(std::span<const Byte>(bytes).first(n)).empty());
                     auto event = decoder.complete();
                     check(event && event->error == FrameError::Incomplete && decoder.buffered_bytes() == 0);
                 }
             }
         }},
        {"short-bodies-are-safe",
         [] {
             for (unsigned n = 0; n < 7; ++n) {
                 Bytes wire{0x3e};
                 wire.insert(wire.end(), n, 0);
                 wire.push_back(0x3c);
                 check(unpack(wire).error == FrameError::BodyTooShort);
             }
         }},
        {"oversized-and-hostile-lengths-bounded",
         [] {
             for (auto length : {65537u, 0xffffffffu, 0x80000000u}) {
                 auto bytes = invalid_declared_length(length, {});
                 check(unpack(bytes).error == FrameError::PayloadTooLarge);
                 StreamDecoder decoder;
                 auto events = decoder.feed(bytes);
                 check(!events.empty() && events[0].error == FrameError::PayloadTooLarge &&
                       decoder.buffered_bytes() == 0);
             }
         }},
        {"exact-local-limit-and-worst-escaping",
         [] {
             Frame frame{table1_type, 0, Bytes(maximum_payload, 0x3e)};
             auto wire = pack(frame);
             check(unpack(wire).frame == frame);
             StreamDecoder decoder;
             check(decoder.feed(wire).at(0).frame == frame);
             throws([&] { pack(frame, maximum_payload - 1); });
         }},
        {"declared-length-mismatch-rejected",
         [] {
             check(unpack(invalid_declared_length(2, {1})).error == FrameError::LengthMismatch);
             check(unpack(invalid_declared_length(0, {1})).error == FrameError::LengthMismatch);
         }},
        {"decoder-reset-clears-session-fragments",
         [] {
             auto bytes = hex(fixtures[0]);
             StreamDecoder decoder;
             decoder.feed(std::span<const Byte>(bytes).first(5));
             decoder.reset();
             check(decoder.feed(std::span<const Byte>(bytes).subspan(5)).empty());
             check(decoder.feed(bytes).at(0).success());
         }},
        {"deterministic-arbitrary-bytes-do-not-crash",
         [] {
             std::mt19937 random(1000);
             for (unsigned n = 0; n < 16000; ++n) {
                 Bytes payload(n % 97);
                 for (auto &b : payload)
                     b = static_cast<Byte>(random());
                 auto frame = Frame{static_cast<Byte>(random() % 3 == 0 ? table2_type : table1_type),
                                    static_cast<Byte>(random() % 3), payload};
                 parse(frame, Family::V2);
                 auto wire = pack(frame);
                 StreamDecoder decoder;
                 decoder.feed(wire);
                 unpack(wire);
             }
         }},
        {"ack-shape-and-sequence-complement",
         [] {
             check(acknowledgement_for(fixture(0)) == Frame{ack_type, 0, {}});
             check(acknowledgement_for(fixture(1)) == Frame{ack_type, 1, {}});
             check(parse(fixture(5)).message.kind == Kind::Acknowledgement);
             check(parse({ack_type, 2, {}}).error == ParseError::InvalidAcknowledgement);
             check(parse({ack_type, 0, {1}}).error == ParseError::InvalidAcknowledgement);
             throws([] { acknowledgement_for({ack_type, 0, {}}); });
         }},
        {"version-and-table-flags-retained",
         [] {
             auto info = value<ProtocolInformation>(fixture(0));
             check(info.family == Family::V2 && info.wire_version == 0x03003018 && info.table1 &&
                   info.table2 == true);
             auto v1 = value<ProtocolInformation>({1, 0, 0x01, 0x02});
             check(v1.family == Family::V1 && v1.wire_version == 0x102 && !v1.table2);
         }},
        {"malformed-protocol-never-guessed",
         [] {
             bad({1});
             bad({1, 1, 0, 0});
             bad({1, 0, 0, 0, 0});
             bad({1, 0, 3, 0, 0x30, 0x18, 0xff, 0});
             bad({1, 0, 3, 0, 0x30, 0x18, 0, 2});
         }},
        {"function-priorities-and-unknown-ids-kept",
         [] {
             auto functions = value<SupportFunctions>({7, 0, 2, 0x6b, 8, 0xab, 0xff});
             check(functions.functions == std::vector<SupportFunction>{{0x6b, 8}, {0xab, 0xff}});
             check(functions.contains(0xab));
         }},
        {"function-count-boundaries",
         [] {
             check(value<SupportFunctions>({7, 0, 0}).functions.empty());
             Bytes p{7, 0, 255};
             for (unsigned i = 0; i < 255; ++i) {
                 p.push_back(static_cast<Byte>(i));
                 p.push_back(0xff);
             }
             check(value<SupportFunctions>(p).functions.size() == 255);
         }},
        {"function-length-validation",
         [] {
             bad({7, 0, 1, 0x20});
             bad({7, 0, 0, 0x20, 0});
             bad({7, 1, 0});
         }},
        {"device-identity-strict-utf8",
         [] {
             check(value<DeviceInformation>(fixture(1)).value == "WF-1000XM5");
             bad({5, 1, 0});
             bad({5, 1, 1, 0xc0});
             bad({5, 1, 3, 0xed, 0xa0, 0x80});
             bad({5, 1, 4, 0xf4, 0x90, 0x80, 0x80});
             check(value<DeviceInformation>({5, 1, 3, 0xe4, 0xb8, 0xad}).value == "中");
         }},
        {"capability-counter-is-metadata",
         [] {
             auto info = value<CapabilityInformation>(fixture(10));
             check(info.counter == 1 && info.identifier == Bytes({'T', 'E', 'S', 'T'}));
             check(value<CapabilityInformation>({3, 0, 0, 0}).identifier.empty());
             bad({3, 0, 1, 1});
         }},
        {"table-aware-message-classification",
         [] {
             check(parse(t1({0x3f, 0}), Family::V2).message.kind == Kind::Notification);
             check(parse(t2({0x3f, 0}), Family::V2).message.kind == Kind::Unknown);
             check(parse(t2({6, 0}), Family::V2).message.kind == Kind::Query);
         }},
        {"unknown-wire-messages-retained",
         [] {
             Frame frame{0xa5, 4, {0xff, 1}};
             auto info = parse(frame);
             check(info.valid() && info.message.frame == frame && info.message.kind == Kind::Unknown);
             check(parse(t1({0x20}), Family::V2).valid());
         }},
        {"no-schema-before-negotiation",
         [] {
             check(parse(fixture(3)).error == ParseError::ProtocolNotNegotiated);
             check(parse(t1({})).error == ParseError::EmptyPayload);
             check(parse(t1({1, 0}, 2)).error == ParseError::InvalidSequence);
         }},
        {"full-upstream-bootstrap",
         [] {
             auto handshake = bootstrap();
             check(handshake.state().phase == Phase::Ready && !handshake.current_query() &&
                   handshake.state().model_name == "WF-1000XM5" && handshake.state().firmware == "6.1.0" &&
                   handshake.state().table2_functions->functions.size() == 11);
         }},
        {"acks-and-notifications-do-not-complete-handshake",
         [] {
             Handshake handshake(42);
             for (const auto &frame : {fixture(5), fixture(11), t1({1, 0})})
                 check(!handshake.receive(42, frame).applied);
             check(handshake.state().phase == Phase::Protocol && handshake.receive(42, fixture(0)).applied);
         }},
        {"duplicate-out-of-order-and-stale-handshake-replies",
         [] {
             Handshake handshake(42);
             check(!handshake.receive(43, fixture(0)).applied);
             check(handshake.receive(42, fixture(0)).applied);
             check(!handshake.receive(42, fixture(0)).applied && !handshake.receive(42, fixture(1)).applied);
             handshake.receive(42, fixture(10));
             check(!handshake.receive(42, fixture(2)).applied);
         }},
        {"disabled-table2-skips-and-maps-absence",
         [] {
             auto handshake = bootstrap(false);
             check(handshake.state().phase == Phase::Ready && !handshake.state().table2_functions);
             auto capabilities = map_capabilities(handshake.state());
             check(capabilities.features.at(Feature::ConnectedSources).access.read == Support::Unsupported);
         }},
        {"v1-and-disabled-table1-explicitly-unsupported",
         [] {
             for (auto frame : {t1({1, 0, 1, 2}), t1({1, 0, 3, 0, 0x30, 0x18, 1, 0})}) {
                 Handshake handshake(42);
                 check(handshake.receive(42, frame).applied &&
                       handshake.state().phase == Phase::Unsupported && !handshake.current_query() &&
                       !handshake.state().error.empty() &&
                       map_capabilities(handshake.state()).features.empty());
             }
         }},
        {"new-handshake-cannot-reuse-old-readings",
         [] {
             auto prior = bootstrap();
             Handshake fresh(43);
             check(!fresh.state().firmware && !fresh.state().protocol &&
                   fresh.state().phase == Phase::Protocol && !fresh.receive(42, fixture(0)).applied);
             throws([] { Handshake invalid(0); });
         }},
        {"advertised-capability-does-not-grant-writes-or-hardware-proof",
         [] {
             auto capabilities = map_capabilities(bootstrap().state());
             check(capabilities.complete && capabilities.readable(Feature::SoundMode));
             auto sound = capabilities.features.at(Feature::SoundMode);
             check(sound.access.write == Support::Unknown && sound.access.notify == Support::Unknown &&
                   !sound.hardware_verified);
             check(!capabilities.readable(Feature::VoiceFocus));
         }},
        {"query-table-and-response-contracts",
         [] {
             check(pack(protocol_info().to_frame(0)) == hex(fixtures[6]));
             check(functions(Table::Table2).type == table2_type);
             check(model_name().payload == Bytes({4, 1}));
             throws([] { protocol_info().to_frame(2); });
             throws([] { functions(static_cast<Table>(99)); });
         }},
        {"apk-static-derived-queries-cross-checked",
         [] {
             // Byte evidence only: artifacts/rea-sony-static-20261013/derived-frames.json.
             // Each example starts independently at sequence 0; this is not a script.
             check(pack(capability_info().to_frame(0)) == hex("3E0C00000000020200103C"));
             check(pack(model_name().to_frame(0)) == hex("3E0C00000000020401133C"));
             check(pack(firmware().to_frame(0)) == hex("3E0C00000000020402143C"));
             check(pack(functions(Table::Table1).to_frame(0)) == hex("3E0C00000000020600143C"));
         }},
        {"local-limit-policy-validated",
         [] {
             throws([] { StreamDecoder decoder(maximum_payload + 1); });
             throws([] { pack({}, maximum_payload + 1); });
             check(unpack(pack({ack_type, 0, {}}, 0), 0).success());
         }},
        {"battery-all-known-subtypes-and-query-selection",
         [] {
             auto functions = SupportFunctions{Table::Table1, {{0x29, 0}, {0x2a, 0}}};
             auto queries = BatteryParameters::queries(&functions, BatteryLayout::LeftRightCase);
             check(queries.size() == 2 && queries[0].payload == Bytes({0x22, 9}) &&
                   queries[1].payload == Bytes({0x22, 10}));
             auto battery = value<BatteryStatus>({0x23, 9, 90, 0, 80, 1, 10, 10});
             check(battery.second->level == 80 && battery.first.charging == 0);
             check(value<BatteryStatus>({0x23, 8, 0xff, 0xff, 10}).first.level == 0xff);
             bad({0x23, 1, 80, 0, 80, 4});
         }},
        {"sound-capability-range-and-availability",
         [] {
             auto capability = value<SoundCapability>({0x61, 0x17, 2, 0, 1, 20, 1, 1, 1, 20, 1});
             check(capability.ranges.at(0).contains(20) && !capability.ranges.at(0).contains(0));
             check(value<SoundAvailability>({0x63, 0x17, 0}).available);
             bad({0x61, 0x17, 1, 0, 20, 1, 1});
             bad({0x61, 0x17, 1, 0, 1, 20, 0});
             bad({0x67, 0x17, 1, 2, 0, 0, 1});
         }},
        {"sound-write-retains-untouched-fields",
         [] {
             auto sound = value<SoundParameter>({0x67, 0x17, 1, 1, 1, 1, 12});
             check(sound.set(SoundMode::Off).payload == Bytes({0x68, 0x17, 1, 0, 1, 1, 12}));
             check(sound.set({}, 20, false).payload == Bytes({0x68, 0x17, 1, 1, 1, 0, 20}));
             check(!sound.set().await_response);
             throws([&] { sound.set({}, 256); });
         }},
        {"equalizer-capability-and-six-step-layout",
         [] {
             auto eq = value<EqualizerCapability>({0x51, 0, 6, 21, 2, 0, 0, 0xa1, 3, 'O', 'n', 'e'});
             check(eq.presets.size() == 2 && eq.presets[0].name == "关闭");
             auto parameter = value<EqualizerParameter>({0x57, 0, 0xa1, 6, 10, 1, 2, 3, 4, 5});
             check(parameter.five_bands() && parameter.bands() == std::vector<int>({-9, -8, -7, -6, -5}));
             bad({0x51, 0, 6, 21, 2, 0, 0, 0, 0});
             bad({0x57, 0, 0, 1});
         }},
        {"equalizer-write-range-and-unknown-layout",
         [] {
             check(EqualizerParameters::set(0xa1, {10, 0, 20, 10, 10, 10}).payload ==
                   Bytes({0x58, 0, 0xa1, 6, 10, 0, 20, 10, 10, 10}));
             throws([] { EqualizerParameters::set(0, {21, 10, 10, 10, 10, 10}); });
             auto unknown = value<EqualizerParameter>({0x57, 0, 0, 2, 10, 20});
             check(!unknown.five_bands());
             throws([&] { unknown.bands(); });
         }},
        {"smart-talking-status-has-independent-effect-field",
         [] {
             auto status = value<ExtendedValue>({0xf3, 2, 0, 1});
             check(status.feature == Feature::SpeakToChat && status.field == ExtendedField::Availability &&
                   status.data == Bytes({0, 1}));
             bad({0xf3, 2, 0});
             bad({0xf3, 2, 0, 2});
         }},
        {"extended-query-and-write-shapes",
         [] {
             const auto &dsee = profile(Feature::Dsee, 1);
             check(ExtendedParameters::write(dsee, 1).payload == Bytes({0xe8, 1, 1}));
             const auto &speak = profile(Feature::SpeakToChat, 2);
             check(ExtendedParameters::write(speak, 0).payload == Bytes({0xf8, 2, 0, 1}));
             const auto &off = profile(Feature::PowerOff, 3);
             check(ExtendedParameters::read_queries(off).empty() &&
                   ExtendedParameters::write(off, 1).payload == Bytes({0x24, 3, 1}));
             throws([&] { ExtendedParameters::read(off); });
         }},
        {"classic-le-capability-has-bounded-distinct-lists",
         [] {
             auto cap = value<ExtendedValue>({0xe1, 5, 3, 0, 1, 2, 1, 0});
             check(cap.feature == Feature::ConnectionPreference && cap.field == ExtendedField::Capability);
             bad({0xe1, 5, 3, 0, 0, 2, 1, 0});
             bad({0xe1, 5, 3, 0, 1, 2, 2, 0});
         }},
        {"dsee-extended-status-and-codec-validated",
         [] {
             check(value<ExtendedValue>({0xe3, 0x0b, 0, 2, 1, 2}).data == Bytes({0, 2, 1, 2}));
             bad({0xe3, 0x0b, 0, 2, 1});
             auto codec = value<ExtendedValue>({0x13, 2, 0x30});
             check(codec.feature == Feature::NegotiatedCodec && codec.field == ExtendedField::Codec);
             bad({0x13, 2, 7});
         }},
        {"auto-power-off-capability-sentinels-readonly",
         [] {
             auto cap = value<ExtendedValue>({0x21, 5, 3, 0, 0x10, 0xff});
             check(cap.data.size() == 4);
             const auto &auto_off = profile(Feature::AutoPowerOff, 5);
             check(ExtendedParameters::write(auto_off, 0x10, 0x11).payload == Bytes({0x28, 5, 0x10, 0x11}));
             throws([&] { ExtendedParameters::write(auto_off, 0xff); });
             bad({0x21, 4, 1, 0x10});
         }},
        {"multipoint-capability-only-explicit-name",
         [] {
             const std::string name = "MULTIPOINT_SETTING";
             Bytes p{0xd1, 0xd1, 0, 1, static_cast<Byte>(name.size())};
             p.insert(p.end(), name.begin(), name.end());
             p.push_back(0);
             check(value<GeneralSettingCapability>(p).multipoint());
             check(AudioSourceParameters::set_multipoint(0xd1, true).payload == Bytes({0xd8, 0xd1, 0, 0}));
             bad({0xd7, 0xd1, 1, 0});
             throws([] { AudioSourceParameters::general_read(0); });
         }},
        {"audio-source-list-address-and-playback",
         [] {
             const std::string id = "AA:BB:CC:DD:EE:01";
             Bytes p{0x37, 2, 1};
             p.insert(p.end(), id.begin(), id.end());
             append(p, {1, 0, 2, 0, 5, 'P', 'h', 'o', 'n', 'e', 1});
             auto sources = value<SourceList>(t2(p));
             check(sources.active_source() == id && sources.entries[0].name == "Phone" &&
                   AudioSourceParameters::category(sources.entries[0].class_of_device) ==
                       SourceCategory::Phone);
             p.back() = 2;
             check(!value<SourceList>(t2(p)).active_source());
             p[3] = 'Z';
             bad2(p);
         }},
        {"source-switch-is-distinct-from-fixed-play",
         [] {
             auto command = AudioSourceParameters::switch_source("aa:bb:cc:dd:ee:01");
             check(command.type == table2_type && command.payload[0] == 0x3c && command.payload[1] == 1 &&
                   command.payload[2] == 'A');
             check(InteractionParameters::fix_write(true).payload == Bytes({0x38, 1, 0}));
             throws([] { AudioSourceParameters::switch_source("invalid"); });
         }},
        {"source-availability-and-alert-layouts",
         [] {
             check(value<SourceCapability>(t2({0x31, 2, 8, 2, 0})).maximum_connected == 2);
             check(value<SourceAvailability>(t2({0x33, 2, 0, 0})).available);
             bad2({0x31, 2, 1, 2, 0});
             check(value<MultipointAlert>({0x99, 0, 6, 2}).message == 6);
             bad({0x99, 0, 6, 3});
         }},
        {"touch-capability-custom-actions-retain-options",
         [] {
             auto cap = value<TouchCapability>(
                 {0xf1, 0x0e, 1, 1, 0, 0, 0x35, 1, 0x35, 1, 1, 0, 0x10, 1, 0x11, 2, 0x11, 0x12});
             check(cap.limitation == 1 && cap.keys.size() == 1 && cap.keys[0].presets[0].functions.empty() &&
                   cap.keys[0].presets[0].customizable[0].functions == Bytes({0x11, 0x12}));
             bad({0xf1, 3, 0});
             bad({0xf1, 3, 1, 0, 0, 0, 1, 0, 2, 0, 0, 1, 0, 2});
         }},
        {"touch-details-parse-and-write",
         [] {
             auto details = value<TouchDetails>({0xfb, 3, 1, 0x35, 2, 0, 1, 1, 2});
             check(details.values[0].actions.size() == 2);
             check(InteractionParameters::touch_details_write(3, details.values).payload ==
                   Bytes({0xfc, 3, 1, 0x35, 2, 0, 1, 1, 2}));
             bad({0xfb, 3, 1, 0x35, 2, 0, 1, 0, 2});
             bad({0xfb, 3, 1, 0x35, 33});
         }},
        {"touch-status-selection-and-write-limits",
         [] {
             check(value<TouchStatus>({0xf3, 3, 2, 0, 0xff}).values.size() == 2);
             check(value<TouchSelection>({0xf7, 3, 2, 0x35, 0xff}).values == Bytes({0x35, 0xff}));
             check(InteractionParameters::touch_write(3, {0x35, 0xff}).payload ==
                   Bytes({0xf8, 3, 2, 0x35, 0xff}));
             bad({0xf3, 3, 1, 2});
             throws([] { InteractionParameters::touch_write(3, {}); });
             check(!InteractionParameters::preset_allowed(0x35, 3));
         }},
        {"voice-assistant-and-explicit-alert-queries",
         [] {
             auto cap = value<VoiceAssistantCapability>({0xf1, 4, 0, 2, 0x30, 0x31});
             check(cap.candidates.size() == 2);
             bad({0xf1, 4, 0, 2, 0x30, 0x30});
             check(InteractionParameters::voice_assistant_write(0x32).payload == Bytes({0xf8, 4, 0x32}));
             check(InteractionParameters::reply_touch_voice_assistant_alert(true).payload ==
                   Bytes({0x98, 0, 0x0f, 1}));
             throws([] { InteractionParameters::voice_assistant_write(0x20); });
         }},
        {"ambient-cycle-range-and-sentinel",
         [] {
             check(value<CycleValue>({0x67, 0x30, 0xff}).value == 0xff);
             bad({0x67, 0x30, 5});
             check(InteractionParameters::cycle_write(4).payload == Bytes({0x68, 0x30, 4}));
             throws([] { InteractionParameters::cycle_write(0); });
         }},
        {"guidance-language-status-and-volume-layouts",
         [] {
             check(value<GuidanceValue>(t2({0x43, 1, 0, 0})).data == Bytes({0, 0}));
             check(value<GuidanceValue>(t2({0x47, 0x20, 0xfe})).data == Bytes({0xfe}));
             bad2({0x47, 0x20, 0xfd});
             bad2({0x43, 1, 1, 0});
             check(InteractionParameters::guidance_write(1, 0, Byte{4}).payload == Bytes({0x48, 1, 0, 4}));
             check(InteractionParameters::volume_write(-2).payload == Bytes({0x48, 0x20, 0xfe, 0}));
             throws([] { InteractionParameters::guidance_write(2, 0, {}); });
         }},
        {"fixed-play-notification-retains-action-result",
         [] {
             auto fixed = value<FixValue>(t2({0x39, 1, 0, 4}));
             check(fixed.fixed && fixed.result == 4);
             bad2({0x39, 1, 0, 5});
             check(value<FixValue>(t2({0x37, 1, 1})).fixed == false);
         }},
        {"all-parameter-prefixes-remain-bounds-safe", [] {
             for (const auto &frame :
                  {t1({0xf1, 3, 1, 0, 0, 0, 1, 0, 1, 0, 0, 1}), t1({0x61, 0x17, 1, 0, 1, 20, 1}),
                   t1({0x51, 0, 6, 21, 1, 0, 3, 'O', 'f', 'f'}), t2({0x41, 1, 0, 0, 0, 0, 0, 2, 1, 2})}) {
                 for (std::size_t n = 0; n <= frame.payload.size(); ++n) {
                     Frame prefix{frame.type, 0, Bytes(frame.payload.begin(), frame.payload.begin() + n)};
                     parse(prefix, Family::V2);
                 }
             }
         }}};
    unsigned failures = 0;
    for (const auto &[name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size()
              << " protocol scenarios passed; HardwareVerified=false\n";
    return failures == 0 ? 0 : 1;
}
