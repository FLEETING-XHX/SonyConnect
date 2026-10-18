#include "sonyconnect/protocol.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sonyconnect::protocol {
namespace {
constexpr Byte start = 0x3e, end = 0x3c, escape = 0x3d;
constexpr std::size_t overhead = 7;
bool one_of(Byte value, std::initializer_list<Byte> options) {
    return std::find(options.begin(), options.end(), value) != options.end();
}
bool status(Byte value) { return one_of(value, {0, 1, 0xff}); }
void validate_limit(std::size_t limit) {
    if (limit > maximum_payload)
        throw std::out_of_range("Invalid local payload limit");
}
FrameResult failed(FrameError error) { return {{}, error}; }
std::uint32_t big_endian(std::span<const Byte> bytes) {
    std::uint32_t value = 0;
    for (auto byte : bytes)
        value = (value << 8) | byte;
    return value;
}
Byte checksum(std::span<const Byte> bytes) {
    Byte value = 0;
    for (auto byte : bytes)
        value = static_cast<Byte>(value + byte);
    return value;
}
FrameResult decode_body(std::span<const Byte> body, std::size_t limit) {
    if (body.size() < overhead)
        return failed(FrameError::BodyTooShort);
    const auto declared = big_endian(body.subspan(2, 4));
    if (declared > limit)
        return failed(FrameError::PayloadTooLarge);
    if (declared != body.size() - overhead)
        return failed(FrameError::LengthMismatch);
    if (checksum(body.first(body.size() - 1)) != body.back())
        return failed(FrameError::BadChecksum);
    return {Frame{body[0], body[1], Bytes(body.begin() + 6, body.end() - 1)}, {}};
}
Bytes tail(std::span<const Byte> payload, std::size_t offset) {
    return Bytes(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
}
bool distinct(std::span<const Byte> bytes) {
    return std::set<Byte>(bytes.begin(), bytes.end()).size() == bytes.size();
}
bool strict_utf8(std::span<const Byte> text) {
    std::size_t at = 0;
    while (at < text.size()) {
        const Byte first = text[at++];
        if (first <= 0x7f)
            continue;
        unsigned count;
        std::uint32_t scalar, minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            count = 1;
            scalar = first & 0x1f;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            count = 2;
            scalar = first & 0x0f;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            count = 3;
            scalar = first & 7;
            minimum = 0x10000;
        } else
            return false;
        if (at + count > text.size())
            return false;
        for (unsigned i = 0; i < count; ++i) {
            const Byte value = text[at++];
            if ((value & 0xc0) != 0x80)
                return false;
            scalar = (scalar << 6) | (value & 0x3f);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            return false;
    }
    return true;
}
std::string utf8(std::span<const Byte> bytes) {
    if (!strict_utf8(bytes))
        throw std::invalid_argument("Malformed UTF-8");
    return std::string(bytes.begin(), bytes.end());
}
bool valid_address(std::string_view text) {
    if (text.size() != 17)
        return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (i % 3 == 2 ? c != ':'
                       : !((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}
std::string address(std::span<const Byte> bytes) {
    std::string value(bytes.begin(), bytes.end());
    if (!valid_address(value))
        throw std::invalid_argument("Invalid audio source address");
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}
Byte valid_slot(Byte slot) {
    if (slot < 0xd1 || slot > 0xd4)
        throw std::out_of_range("Invalid general setting slot");
    return slot;
}
Byte valid_subtype(Byte type) {
    if (type != 0 && type != 2)
        throw std::out_of_range("Invalid source subtype");
    return type;
}
struct Cursor {
    std::span<const Byte> bytes;
    std::size_t offset{};
    Byte take() {
        if (offset >= bytes.size())
            throw std::invalid_argument("Truncated payload");
        return bytes[offset++];
    }
    std::span<const Byte> take(std::size_t count) {
        if (count > bytes.size() - offset)
            throw std::invalid_argument("Truncated payload");
        auto value = bytes.subspan(offset, count);
        offset += count;
        return value;
    }
    std::string string() {
        const auto count = take();
        return utf8(take(count));
    }
    void finish() const {
        if (offset != bytes.size())
            throw std::invalid_argument("Trailing payload");
    }
};
ParseResult unknown(const Frame &frame, std::optional<ParseError> error = {}) {
    return {
        Message{frame, Kind::Unknown, frame.payload.empty() ? std::optional<Byte>{} : frame.payload[0], {}},
        error};
}
ParseResult malformed(const Frame &frame) { return unknown(frame, ParseError::MalformedPayload); }
ParseResult result(const Frame &frame, Kind kind, Information info = {}) {
    return {{frame, kind, frame.payload[0], std::move(info)}, {}};
}
Kind regular_kind(Byte command) {
    return command % 16 == 5 || command % 16 == 9 ? Kind::Notification : Kind::Response;
}

std::optional<ParseResult> parse_battery(const Frame &frame) {
    const auto &p = frame.payload;
    if (frame.type != table1_type || p.empty() || !one_of(p[0], {0x23, 0x25}))
        return {};
    if (p.size() < 2)
        return malformed(frame);
    const auto length = p[1] == 0 || p[1] == 2    ? 4
                        : p[1] == 1               ? 6
                        : p[1] == 8 || p[1] == 10 ? 5
                        : p[1] == 9               ? 8
                                                  : 0;
    if (length == 0)
        return {};
    if (p.size() != static_cast<std::size_t>(length) || !one_of(p[3], {0, 1, 2, 3, 0xff}) ||
        ((p[1] == 1 || p[1] == 9) && !one_of(p[5], {0, 1, 2, 3, 0xff})))
        return malformed(frame);
    BatteryStatus value{p[1], {p[2], p[3]}, {}};
    if (p[1] == 1 || p[1] == 9)
        value.second = BatteryUnit{p[4], p[5]};
    return result(frame, p[0] == 0x23 ? Kind::Response : Kind::Notification, value);
}
std::optional<ParseResult> parse_sound(const Frame &frame) {
    const auto &p = frame.payload;
    if (frame.type != table1_type || p.size() < 2 || p[1] != 0x17 ||
        !one_of(p[0], {0x61, 0x63, 0x65, 0x67, 0x69}))
        return {};
    Information information;
    if (p[0] == 0x61) {
        if (p.size() < 3 || p.size() != 3 + 4u * p[2])
            return malformed(frame);
        SoundCapability value;
        for (std::size_t i = 3; i < p.size(); i += 4) {
            if (p[i] > 1 || p[i + 1] > p[i + 2] || p[i + 3] == 0 || value.ranges.contains(p[i]))
                return malformed(frame);
            value.ranges.emplace(p[i], AmbientRange{p[i + 1], p[i + 2], p[i + 3]});
        }
        information = value;
    } else if (p[0] == 0x63 || p[0] == 0x65) {
        if (p.size() != 3 || p[2] > 1)
            return malformed(frame);
        information = SoundAvailability{p[2] == 0};
    } else {
        if (p.size() != 7 || p[2] > 1 || p[3] > 1 || p[4] > 1 || p[5] > 1)
            return malformed(frame);
        information = SoundParameter{p[2] == 1, p[3] == 1, p[4], p[5], p[6]};
    }
    return result(frame, regular_kind(p[0]), information);
}
std::optional<ParseResult> parse_equalizer(const Frame &frame) {
    const auto &p = frame.payload;
    if (frame.type != table1_type || p.size() < 2 || p[1] != 0 ||
        !one_of(p[0], {0x51, 0x53, 0x55, 0x57, 0x59}))
        return {};
    Information information;
    if (p[0] == 0x51) {
        if (p.size() < 5 || p[2] > 127 || p[3] > 127)
            return malformed(frame);
        EqualizerCapability value{p[2], p[3], {}};
        Cursor cursor{p, 5};
        std::set<Byte> ids;
        for (unsigned n = 0; n < p[4]; ++n) {
            const auto id = cursor.take();
            auto name = cursor.string();
            if (!ids.insert(id).second)
                return malformed(frame);
            if (name.empty() ||
                std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isspace(c) != 0; }))
                name = EqualizerParameters::preset_name(id);
            value.presets.push_back({id, std::move(name)});
        }
        cursor.finish();
        information = value;
    } else if (p[0] == 0x53 || p[0] == 0x55) {
        if (p.size() != 3 || p[2] > 1)
            return malformed(frame);
        information = EqualizerAvailability{p[2] == 0};
    } else {
        if (p.size() < 4 || p.size() != 4u + p[3])
            return malformed(frame);
        information = EqualizerParameter{p[2], tail(p, 4)};
    }
    return result(frame, regular_kind(p[0]), information);
}
bool readable(const ExtendedProfile &profile, Byte value) {
    return ExtendedParameters::legal(profile, value) ||
           (value == 0xff && profile.feature != Feature::PowerOff) ||
           (profile.feature == Feature::ConnectionPreference && value == 2);
}
bool valid_classic_le_capability(std::span<const Byte> p) {
    if (p.size() < 6 || p[2] < 2 || p[2] > 32)
        return false;
    const auto exclusive = 3u + p[2];
    if (exclusive >= p.size() || p[exclusive] > 32 || p.size() != exclusive + 1u + p[exclusive])
        return false;
    const auto modes = p.subspan(3, p[2]), features = p.subspan(exclusive + 1);
    return distinct(modes) &&
           std::all_of(modes.begin(), modes.end(), [](Byte v) { return one_of(v, {0, 1, 2, 0xff}); }) &&
           distinct(features) &&
           std::all_of(features.begin(), features.end(), [](Byte v) { return v == 0 || v == 0xff; });
}
std::optional<ParseResult> parse_extended(const Frame &frame) {
    std::span<const Byte> p = frame.payload;
    if (frame.type != table1_type || p.size() < 2)
        return {};
    const auto command = p[0], type = p[1];
    if ((command == 0x13 || command == 0x15) && type == 2) {
        if (p.size() != 3 || !one_of(p[2], {0, 1, 2, 0x10, 0x20, 0x21, 0x30, 0xff}))
            return malformed(frame);
        return result(frame, regular_kind(command),
                      ExtendedValue{Feature::NegotiatedCodec, 0x10, type, ExtendedField::Codec, {p[2]}});
    }
    const auto &profiles = ExtendedParameters::profiles();
    const auto found = std::find_if(profiles.begin(), profiles.end(), [&](const auto &profile) {
        return (command & 0xf0) == profile.group &&
               (type == profile.type || (profile.feature == Feature::Dsee && type == 0x0b));
    });
    if (found == profiles.end() || !one_of(static_cast<Byte>(command % 16), {1, 3, 5, 7, 9}))
        return {};
    const auto &profile = *found;
    const auto field = command % 16 == 1                        ? ExtendedField::Capability
                       : command % 16 == 3 || command % 16 == 5 ? ExtendedField::Availability
                                                                : ExtendedField::Parameter;
    if (field == ExtendedField::Capability) {
        bool valid;
        if (profile.feature == Feature::Dsee)
            valid = p.size() == 3 && (p[2] <= 3 || p[2] == 0xff);
        else if (profile.feature == Feature::SpeakToChat)
            valid = p.size() == 6 && one_of(p[2], {0, 1, 0x7f}) && p[3] > 0 && p[4] > 0 && p[5] > 0;
        else if (profile.feature == Feature::AutoPowerOff)
            valid = p.size() >= 3 && p.size() == 3u + p[2] && p[2] > 0 && distinct(p.subspan(3)) &&
                    std::all_of(p.begin() + 3, p.end(), [&](Byte v) { return readable(profile, v); });
        else if (profile.feature == Feature::ConnectionPreference && type == 5)
            valid = valid_classic_le_capability(p);
        else
            valid = p.size() == 2;
        if (!valid)
            return malformed(frame);
    } else if (field == ExtendedField::Availability) {
        if (profile.feature == Feature::SpeakToChat) {
            if (p.size() != 4 || !status(p[2]) || !status(p[3]))
                return malformed(frame);
        } else if (profile.feature == Feature::ConnectionPreference && (type == 2 || type == 5)) {
            if ((p.size() != 3 && p.size() != 4) || (type == 5 && p.size() != 4) || !status(p[2]) ||
                (p.size() == 4 && !status(p[3])))
                return malformed(frame);
        } else {
            if (type == 0x0b ? p.size() < 4 || p.size() != 4u + p[3] : p.size() != 3)
                return malformed(frame);
            if (!status(p[2]))
                return malformed(frame);
        }
    } else {
        if (profile.feature == Feature::ConnectionPreference && type == 5) {
            if ((p.size() != 3 && p.size() != 4) || !readable(profile, p[2]) ||
                (p.size() == 4 && !one_of(p[3], {0, 1, 2, 0xff})))
                return malformed(frame);
        } else {
            const auto length =
                profile.feature == Feature::SpeakToChat || profile.feature == Feature::AutoPowerOff ? 4u : 3u;
            if (profile.feature == Feature::PowerOff || p.size() != length || !readable(profile, p[2]))
                return malformed(frame);
            if ((profile.feature == Feature::SpeakToChat && !status(p[3])) ||
                (profile.feature == Feature::AutoPowerOff && !readable(profile, p[3])))
                return malformed(frame);
        }
    }
    return result(frame, regular_kind(command),
                  ExtendedValue{profile.feature, profile.group, type, field, tail(p, 2)});
}
std::optional<ParseResult> parse_sources(const Frame &frame) {
    std::span<const Byte> p = frame.payload;
    if (p.size() < 2)
        return {};
    if (frame.type == table1_type && p[0] == 0x99 && p[1] == 0) {
        if (p.size() != 4 || p[3] > 2)
            return malformed(frame);
        return result(frame, Kind::Notification, MultipointAlert{p[2], p[3]});
    }
    const bool gs = frame.type == table1_type && p[1] >= 0xd1 && p[1] <= 0xd4 &&
                    one_of(p[0], {0xd1, 0xd3, 0xd5, 0xd7, 0xd9});
    const bool peri = frame.type == table2_type &&
                      (((p[1] == 0 || p[1] == 2) && one_of(p[0], {0x31, 0x33, 0x35, 0x37, 0x39})) ||
                       (p[1] == 1 && p[0] == 0x3d));
    if (!gs && !peri)
        return {};
    Information information;
    if (gs) {
        if (p[0] == 0xd1) {
            if (p.size() < 6 || p[2] > 1 || p[3] > 1)
                return malformed(frame);
            if (p[2] == 1)
                return result(frame, Kind::Response); // LIST tail intentionally uninterpreted.
            Cursor cursor{p, 4};
            auto name = cursor.string();
            cursor.string();
            cursor.finish();
            information = GeneralSettingCapability{p[1], true, p[3], std::move(name)};
        } else if (p[0] == 0xd3 || p[0] == 0xd5) {
            if (p.size() != 3 || p[2] > 1)
                return malformed(frame);
            information = GeneralSettingAvailability{p[1], p[2] == 0};
        } else {
            if (p.size() != 4 || p[2] != 0 || p[3] > 1)
                return malformed(frame);
            information = GeneralSettingParameter{p[1], p[3] == 0};
        }
    } else if (p[0] == 0x31) {
        if (p.size() != 5 || p[3] > p[2] || p[4] > 1)
            return malformed(frame);
        information = SourceCapability{p[1], p[2], p[3]};
    } else if (p[0] == 0x3d) {
        if (p.size() != 20 || p[2] > 4)
            return malformed(frame);
        information = SourceSwitchResult{p[2], address(p.subspan(3))};
    } else if (p[0] == 0x33 || p[0] == 0x35) {
        if (p.size() != 4 || p[2] > 1 || p[3] > 1)
            return malformed(frame);
        information = SourceAvailability{p[1], p[3] == 0 && p[2] == 0};
    } else {
        if (p.size() < 4)
            return malformed(frame);
        Cursor cursor{p, 3};
        SourceList value{p[1], {}, 0};
        std::set<std::string> ids;
        std::set<Byte> slots;
        for (unsigned i = 0; i < p[2]; ++i) {
            auto id = address(cursor.take(17));
            const auto slot = cursor.take();
            if (!ids.insert(id).second || (slot != 0 && !slots.insert(slot).second))
                return malformed(frame);
            std::optional<int> cod;
            if (p[1] == 2)
                cod = static_cast<int>(big_endian(cursor.take(3)));
            auto name = cursor.string();
            value.entries.push_back({std::move(id), std::move(name), slot, cod});
        }
        value.playback_slot = cursor.take();
        cursor.finish();
        information = value;
    }
    return result(frame, one_of(p[0], {0xd5, 0xd9, 0x35, 0x39, 0x3d}) ? Kind::Notification : Kind::Response,
                  information);
}
std::optional<ParseResult> parse_interaction(const Frame &frame) {
    std::span<const Byte> p = frame.payload;
    if (p.size() < 2)
        return {};
    if (frame.type == table1_type && p[1] == 4 && one_of(p[0], {0xf1, 0xf7, 0xf9})) {
        if (p[0] == 0xf1 && p.size() >= 4 && p[3] > 0 && p[3] <= 32 && p.size() == 4u + p[3]) {
            auto candidates = tail(p, 4);
            if (!distinct(candidates))
                return malformed(frame);
            return result(frame, Kind::Response, VoiceAssistantCapability{p[2], std::move(candidates)});
        }
        if ((p[0] == 0xf7 || p[0] == 0xf9) && p.size() == 3)
            return result(frame, regular_kind(p[0]), VoiceAssistantValue{p[2]});
        return malformed(frame);
    }
    const bool touch = frame.type == table1_type && (p[1] == 3 || p[1] == 0x0e) &&
                       one_of(p[0], {0xf1, 0xf3, 0xf5, 0xf7, 0xf9, 0xfb, 0xfd});
    const bool cycle =
        frame.type == table1_type && p[1] == 0x30 && one_of(p[0], {0x61, 0x63, 0x65, 0x67, 0x69});
    const bool guidance = frame.type == table2_type && (p[1] <= 3 || p[1] == 0x20 || p[1] == 0x21) &&
                          one_of(p[0], {0x41, 0x43, 0x45, 0x47, 0x49});
    const bool fix = frame.type == table2_type && p[1] == 1 && (p[0] == 0x37 || p[0] == 0x39);
    if (!touch && !cycle && !guidance && !fix)
        return {};
    Information information;
    if (touch) {
        Cursor cursor{p, 2};
        if (p[0] == 0xf1) {
            TouchCapability value{p[1], static_cast<Byte>(p[1] == 0x0e ? cursor.take() : 0), {}};
            const auto count = cursor.take();
            if (count < 1 || count > 6)
                return malformed(frame);
            std::set<Byte> key_ids;
            for (unsigned i = 0; i < count; ++i) {
                TouchKey key;
                key.id = cursor.take();
                key.type = cursor.take();
                key.default_preset = cursor.take();
                if ((key.id > 5 && key.id != 0xff) || (key.type > 2 && key.type != 0xff) ||
                    !key_ids.insert(key.id).second)
                    return malformed(frame);
                const auto preset_count = cursor.take();
                if (preset_count < 1 || preset_count > 32)
                    return malformed(frame);
                std::set<Byte> preset_ids;
                for (unsigned j = 0; j < preset_count; ++j) {
                    TouchPreset preset;
                    preset.id = cursor.take();
                    const auto actions = cursor.take(), customizable = cursor.take();
                    if (!preset_ids.insert(preset.id).second || actions > 32 || customizable > 32)
                        return malformed(frame);
                    std::set<Byte> action_ids;
                    for (unsigned a = 0; a < actions; ++a) {
                        const auto action = cursor.take(), function = cursor.take();
                        if (!action_ids.insert(action).second)
                            return malformed(frame);
                        preset.actions.push_back({action, function});
                        if (action == 0)
                            preset.functions.push_back(function);
                    }
                    for (unsigned a = 0; a < customizable; ++a) {
                        const auto action = cursor.take(), default_function = cursor.take(),
                                   n = cursor.take();
                        if (n > 64 || !action_ids.insert(action).second)
                            return malformed(frame);
                        auto options = cursor.take(n);
                        preset.customizable.push_back(
                            {action, default_function, Bytes(options.begin(), options.end())});
                    }
                    // Defaults do not prove current customized actions; extended readback is required.
                    if (customizable != 0)
                        preset.functions.clear();
                    key.presets.push_back(std::move(preset));
                }
                value.keys.push_back(std::move(key));
            }
            information = std::move(value);
        } else if (p[0] == 0xfb || p[0] == 0xfd) {
            const auto count = cursor.take();
            if (count < 1 || count > 32)
                return malformed(frame);
            TouchDetails value{p[1], {}};
            std::set<Byte> presets;
            for (unsigned i = 0; i < count; ++i) {
                TouchPresetState preset;
                preset.preset = cursor.take();
                const auto action_count = cursor.take();
                if (action_count > 32 || !presets.insert(preset.preset).second)
                    return malformed(frame);
                std::set<Byte> ids;
                for (unsigned j = 0; j < action_count; ++j) {
                    const auto action = cursor.take(), function = cursor.take();
                    if (!ids.insert(action).second)
                        return malformed(frame);
                    preset.actions.push_back({action, function});
                }
                value.values.push_back(std::move(preset));
            }
            information = std::move(value);
        } else {
            const auto count = cursor.take();
            if (count < 1 || count > 6)
                return malformed(frame);
            const auto bytes = cursor.take(count);
            Bytes values(bytes.begin(), bytes.end());
            if (p[0] == 0xf3 || p[0] == 0xf5) {
                if (!std::all_of(values.begin(), values.end(), status))
                    return malformed(frame);
                information = TouchStatus{p[1], std::move(values)};
            } else
                information = TouchSelection{p[1], std::move(values)};
        }
        cursor.finish();
    } else if (cycle) {
        if (p.size() != 3 || (p[0] == 0x61 && p[2] > 5 && p[2] != 0xff) ||
            ((p[0] == 0x63 || p[0] == 0x65) && !status(p[2])) ||
            ((p[0] == 0x67 || p[0] == 0x69) && p[2] > 4 && p[2] != 0xff))
            return malformed(frame);
        information = CycleValue{p[0], p[2]};
    } else if (guidance) {
        const auto type = p[1];
        if (p[0] == 0x41) {
            if ((type == 0 && p.size() != 7) || (type == 1 && (p.size() < 8 || p.size() != 8u + p[7])) ||
                (type == 2 && (p.size() < 4 || p.size() != 4u + p[3])) ||
                (one_of(type, {3, 0x20, 0x21}) && p.size() != 2))
                return malformed(frame);
            if (((type == 0 || type == 1) &&
                 (!status(p[3]) || !status(p[4]) || !status(p[5]) || !status(p[6]))) ||
                (type == 2 && !status(p[2])))
                return malformed(frame);
        } else if (p[0] == 0x43 || p[0] == 0x45) {
            const auto at = type <= 2 ? 3u : 2u;
            if (p.size() != at + 1 || (at == 3 && p[2] != 0) || !status(p[at]))
                return malformed(frame);
        } else {
            const auto length = type == 1 || type == 2 ? 4u : 3u;
            const auto signed_value =
                p.size() >= 3 ? (p[2] >= 128 ? static_cast<int>(p[2]) - 256 : static_cast<int>(p[2])) : 0;
            if (p.size() != length || (type <= 3 && !status(p[2])) ||
                (type >= 0x20 && (signed_value < -2 || signed_value > 2)))
                return malformed(frame);
        }
        information = GuidanceValue{p[0], type, tail(p, 2)};
    } else {
        if (p.size() != (p[0] == 0x39 ? 4u : 3u) || p[2] > 1 || (p.size() == 4 && p[3] > 4 && p[3] != 0xff))
            return malformed(frame);
        information = FixValue{p[2] == 0, p.size() == 4 ? std::optional<Byte>{p[3]} : std::nullopt};
    }
    return result(frame,
                  one_of(p[0], {0xf5, 0xf9, 0xfd, 0x65, 0x69, 0x45, 0x49, 0x39}) ? Kind::Notification
                                                                                 : Kind::Response,
                  information);
}
std::optional<ParseResult> parse_safe_listening(const Frame &frame) {
    const auto &p = frame.payload;
    if (frame.type != table2_type || p.empty() || !one_of(p[0], {0x51, 0x57, 0x59, 0x5b}))
        return {};
    if (p.size() < 2)
        return malformed(frame);
    // Volume-limit types 4/5 use other layouts; never interpret them as sound pressure.
    if (p[1] > 3)
        return result(frame, p[0] == 0x59 ? Kind::Notification : Kind::Response);
    if (p[0] == 0x51) {
        if (p.size() != 9)
            return malformed(frame);
        return result(frame, Kind::Response,
                      SafeListeningCapability{p[1], p[2], p[7], p[8], big_endian(std::span(p).subspan(3, 4))});
    }
    if (p[0] == 0x57) {
        if (p.size() != 3 || p[2] > 1)
            return malformed(frame);
        return result(frame, Kind::Response, SafeListeningAvailability{p[1], p[2] == 0});
    }
    if (p[0] == 0x59) {
        if (p.size() != 4 || p[2] > 1 || p[3] > 1)
            return malformed(frame);
        return result(frame, Kind::Notification, SafeListeningMode{p[1], p[2] == 0, p[3] == 0});
    }
    if (p.size() != 4 || (p[2] == 0xff && p[3] != 0 && p[3] != 1 && p[3] != 2))
        return malformed(frame);
    return result(frame, Kind::Response, SoundPressure{p[1], p[2], p[3]});
}
Kind classify(Byte table, Byte command) {
    if (table == table1_type) {
        if (one_of(command, {0x15, 0x19, 0x25, 0x29, 0x35, 0x39, 0x3f, 0x45, 0x49, 0x4d, 0x55,
                             0x59, 0x65, 0x69, 0x75, 0x79, 0x85, 0x89, 0x95, 0x99, 0xa5, 0xa9,
                             0xb5, 0xb9, 0xc9, 0xd5, 0xd9, 0xe5, 0xe9, 0xf5, 0xf9, 0xfd}))
            return Kind::Notification;
        if (one_of(command, {0x11, 0x13, 0x21, 0x23, 0x27, 0x51, 0x53, 0x57, 0x61, 0x63, 0x67, 0xa1,
                             0xa3, 0xa7, 0xd1, 0xd3, 0xd7, 0xe1, 0xe3, 0xe7, 0xf1, 0xf3, 0xf7}))
            return Kind::Response;
        if (one_of(command, {0, 2, 4, 6}))
            return Kind::Query;
    } else if (table == table2_type) {
        if (one_of(command, {0x25, 0x29, 0x35, 0x39, 0x3d, 0x45, 0x55, 0x59, 0x65, 0x69, 0x75, 0x79, 0xf5,
                             0xf9, 0xfd}))
            return Kind::Notification;
        if (one_of(command, {0x21, 0x23, 0x27, 0x31, 0x33, 0x37, 0x41, 0x43, 0x47, 0x53}))
            return Kind::Response;
        if (command == 6)
            return Kind::Query;
    }
    return Kind::Unknown;
}
} // namespace

Bytes pack(const Frame &frame, std::size_t limit) {
    validate_limit(limit);
    if (frame.payload.size() > limit)
        throw std::out_of_range("Payload exceeds local memory policy");
    const auto length = static_cast<std::uint32_t>(frame.payload.size());
    Bytes body{frame.type,
               frame.sequence,
               static_cast<Byte>(length >> 24),
               static_cast<Byte>(length >> 16),
               static_cast<Byte>(length >> 8),
               static_cast<Byte>(length)};
    body.insert(body.end(), frame.payload.begin(), frame.payload.end());
    body.push_back(checksum(body));
    Bytes wire{start};
    wire.reserve(2 * body.size() + 2);
    for (const auto value : body) {
        if (value == start || value == end || value == escape) {
            wire.push_back(escape);
            wire.push_back(static_cast<Byte>(value - 0x10));
        } else
            wire.push_back(value);
    }
    wire.push_back(end);
    return wire;
}
FrameResult unpack(std::span<const Byte> wire, std::size_t limit) {
    validate_limit(limit);
    if (wire.size() < 2)
        return failed(FrameError::Incomplete);
    if (wire.front() != start || wire.back() != end)
        return failed(FrameError::BadMarker);
    if (wire.size() > 2 * (limit + overhead) + 2)
        return failed(FrameError::PayloadTooLarge);
    Bytes body;
    body.reserve(std::min(wire.size(), limit + overhead));
    for (std::size_t i = 1; i < wire.size() - 1; ++i) {
        auto value = wire[i];
        if (value == escape) {
            if (++i >= wire.size() - 1 || wire[i] < 0x2c || wire[i] > 0x2e)
                return failed(FrameError::InvalidEscape);
            value = static_cast<Byte>(wire[i] + 0x10);
        } else if (value == start || value == end)
            return failed(FrameError::BadMarker);
        if (body.size() == limit + overhead)
            return failed(FrameError::PayloadTooLarge);
        body.push_back(value);
    }
    return decode_body(body, limit);
}
StreamDecoder::StreamDecoder(std::size_t limit) : limit_(limit) { validate_limit(limit); }
void StreamDecoder::reset() {
    body_.clear();
    started_ = escaped_ = false;
    expected_.reset();
}
std::vector<FrameResult> StreamDecoder::feed(std::span<const Byte> chunk) {
    std::vector<FrameResult> output;
    for (auto value : chunk) {
        if (value == start) {
            if (started_)
                output.push_back(failed(FrameError::InterruptedFrame));
            reset();
            started_ = true;
            continue;
        }
        if (!started_)
            continue;
        if (value == end) {
            output.push_back(escaped_ ? failed(FrameError::InvalidEscape) : decode_body(body_, limit_));
            reset();
            continue;
        }
        if (!escaped_ && value == escape) {
            escaped_ = true;
            continue;
        }
        if (escaped_) {
            if (value < 0x2c || value > 0x2e) {
                output.push_back(failed(FrameError::InvalidEscape));
                reset();
                continue;
            }
            value = static_cast<Byte>(value + 0x10);
            escaped_ = false;
        }
        body_.push_back(value);
        if (body_.size() == 6) {
            const auto length = big_endian(std::span<const Byte>(body_).subspan(2, 4));
            if (length > limit_) {
                output.push_back(failed(FrameError::PayloadTooLarge));
                reset();
                continue;
            }
            expected_ = static_cast<std::size_t>(length) + overhead;
        }
        if (body_.size() > limit_ + overhead || (expected_ && body_.size() > *expected_)) {
            output.push_back(failed(FrameError::LengthMismatch));
            reset();
        }
    }
    return output;
}
std::optional<FrameResult> StreamDecoder::complete() {
    auto value = started_ ? std::optional<FrameResult>{failed(FrameError::Incomplete)} : std::nullopt;
    reset();
    return value;
}
Frame Query::to_frame(Byte sequence) const {
    if (sequence > 1)
        throw std::out_of_range("MDR sequence must be 0 or 1");
    return {type, sequence, payload};
}
Query read_query(Byte table, Byte command, Byte subtype) {
    return {table, {command, subtype}, static_cast<Byte>(command + 1), subtype, true};
}
Query write_query(Byte table, Bytes payload) { return {table, std::move(payload), 0, 0, false}; }
Query protocol_info() { return read_query(table1_type, 0, 0); }
Query capability_info() { return read_query(table1_type, 2, 0); }
Query model_name() { return read_query(table1_type, 4, 1); }
Query firmware() { return read_query(table1_type, 4, 2); }
Query functions(Table table) {
    if (table != Table::Table1 && table != Table::Table2)
        throw std::out_of_range("Invalid MDR table");
    return read_query(table == Table::Table1 ? table1_type : table2_type, 6, 0);
}
Frame acknowledgement_for(const Frame &frame) {
    if (!frame.requires_acknowledgement() || frame.sequence > 1)
        throw std::invalid_argument("Only MDR table data with sequence 0/1 requires ACK");
    return {ack_type, static_cast<Byte>(1 - frame.sequence), {}};
}
bool SupportFunctions::contains(Byte id) const {
    return std::any_of(functions.begin(), functions.end(), [id](auto function) { return function.id == id; });
}
bool AmbientRange::contains(int value) const {
    return step > 0 && value >= minimum && value <= maximum && (value - minimum) % step == 0;
}
SoundMode SoundParameter::sound_mode() const {
    return !enabled ? SoundMode::Off : mode == 0 ? SoundMode::NoiseCanceling : SoundMode::Ambient;
}
Query SoundParameter::set(std::optional<SoundMode> target, std::optional<int> target_level,
                          std::optional<bool> voice_focus) const {
    if (target && *target != SoundMode::Off && *target != SoundMode::Ambient &&
        *target != SoundMode::NoiseCanceling)
        throw std::out_of_range("Invalid sound mode");
    if (target_level && (*target_level < 0 || *target_level > 255))
        throw std::out_of_range("Invalid ambient level");
    const Byte target_enabled = static_cast<Byte>(target ? *target != SoundMode::Off : enabled);
    const Byte target_mode = !target || *target == SoundMode::Off
                                 ? mode
                                 : static_cast<Byte>(*target == SoundMode::NoiseCanceling ? 0 : 1);
    return write_query(table1_type, {0x68, 0x17, 1, target_enabled, target_mode,
                                     static_cast<Byte>(voice_focus ? *voice_focus : ambient_mode),
                                     static_cast<Byte>(target_level.value_or(level))});
}
bool EqualizerParameter::five_bands() const {
    return steps.size() == 6 && std::all_of(steps.begin(), steps.end(), [](Byte v) { return v <= 20; });
}
std::vector<int> EqualizerParameter::bands() const {
    if (!five_bands())
        throw std::logic_error("Unconfirmed five-band EQ layout");
    std::vector<int> values;
    for (std::size_t i = 1; i < steps.size(); ++i)
        values.push_back(steps[i] - 10);
    return values;
}
Byte ExtendedProfile::status_type() const {
    return feature == Feature::Dsee && function == 0xed ? static_cast<Byte>(0x0b) : type;
}
bool GeneralSettingCapability::multipoint() const {
    return boolean && string_format == 1 && name == "MULTIPOINT_SETTING";
}
std::optional<std::string> SourceList::active_source() const {
    if (playback_slot == 0)
        return {};
    std::optional<std::string> found;
    for (const auto &entry : entries)
        if (entry.connection_slot == playback_slot) {
            if (found)
                return {};
            found = entry.id;
        }
    return found;
}

// Parameter constructors retain the C# request and readback contract byte-for-byte.
std::vector<Query> BatteryParameters::queries(const SupportFunctions *functions, BatteryLayout layout) {
    std::vector<Query> output;
    auto add = [&](Byte plain, Byte threshold, Byte subtype) {
        if (functions && functions->contains(plain))
            output.push_back(read_query(table1_type, 0x22, subtype));
        else if (functions && functions->contains(threshold))
            output.push_back(read_query(table1_type, 0x22, static_cast<Byte>(subtype + 8)));
    };
    if (layout == BatteryLayout::Single)
        add(0x20, 0x28, 0);
    if (layout == BatteryLayout::LeftRightCase) {
        add(0x21, 0x29, 1);
        add(0x22, 0x2a, 2);
    }
    return output;
}
Query SoundParameters::capability() { return read_query(table1_type, 0x60, 0x17); }
Query SoundParameters::availability() { return read_query(table1_type, 0x62, 0x17); }
Query SoundParameters::read() { return read_query(table1_type, 0x66, 0x17); }
Query EqualizerParameters::capability() { return {table1_type, {0x50, 0, 1}, 0x51, 0, true}; }
Query EqualizerParameters::availability() { return read_query(table1_type, 0x52, 0); }
Query EqualizerParameters::read() { return read_query(table1_type, 0x56, 0); }
Query EqualizerParameters::set(Byte preset, const Bytes &steps) {
    if (!steps.empty() &&
        (steps.size() != 6 || std::any_of(steps.begin(), steps.end(), [](Byte v) { return v > 20; })))
        throw std::out_of_range("Invalid EQ steps");
    Bytes payload{0x58, 0, preset, static_cast<Byte>(steps.size())};
    payload.insert(payload.end(), steps.begin(), steps.end());
    return write_query(table1_type, std::move(payload));
}
std::string EqualizerParameters::preset_name(Byte id) {
    switch (id) {
    case 0:
        return "关闭";
    case 0x10:
        return "欢快";
    case 0x11:
        return "激昂";
    case 0x12:
        return "醇美";
    case 0x13:
        return "放松";
    case 0x14:
        return "原声";
    case 0x15:
        return "高音增强";
    case 0x16:
        return "低音增强";
    case 0x17:
        return "演说";
    case 0xa0:
        return "手动";
    case 0xa1:
        return "自定义1";
    case 0xa2:
        return "自定义2";
    default:
        std::ostringstream text;
        text << "预设 0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
             << static_cast<unsigned>(id);
        return text.str();
    }
}
const std::vector<ExtendedProfile> &ExtendedParameters::profiles() {
    static const std::vector<ExtendedProfile> values{{Feature::ConnectionPreference, 0xe1, 0xe0, 0},
                                                     {Feature::ConnectionPreference, 0xe3, 0xe0, 2},
                                                     {Feature::ConnectionPreference, 0xe7, 0xe0, 5},
                                                     {Feature::Dsee, 0xe2, 0xe0, 1},
                                                     {Feature::Dsee, 0xed, 0xe0, 1},
                                                     {Feature::SpeakToChat, 0xf2, 0xf0, 2},
                                                     {Feature::SpeakToChat, 0xfc, 0xf0, 0x0c},
                                                     {Feature::WearDetectionPause, 0xf1, 0xf0, 1},
                                                     {Feature::AutoPowerOff, 0x24, 0x20, 4},
                                                     {Feature::AutoPowerOff, 0x25, 0x20, 5},
                                                     {Feature::PowerOff, 0x23, 0x20, 3}};
    return values;
}
Query ExtendedParameters::capability(const ExtendedProfile &profile) {
    if (profile.feature == Feature::WearDetectionPause || profile.feature == Feature::PowerOff)
        throw std::logic_error("No separate capability query");
    return read_query(table1_type, profile.group, profile.type);
}
Query ExtendedParameters::availability(const ExtendedProfile &profile) {
    if (profile.feature == Feature::WearDetectionPause || profile.feature == Feature::PowerOff)
        throw std::logic_error("No separate availability query");
    return read_query(table1_type, static_cast<Byte>(profile.group + 2), profile.status_type());
}
Query ExtendedParameters::read(const ExtendedProfile &profile) {
    if (profile.feature == Feature::PowerOff)
        throw std::logic_error("Power-off has no readable parameter");
    return read_query(table1_type, static_cast<Byte>(profile.group + 6), profile.type);
}
std::vector<Query> ExtendedParameters::read_queries(const ExtendedProfile &profile) {
    if (profile.feature == Feature::PowerOff)
        return {};
    if (profile.feature == Feature::WearDetectionPause)
        return {read(profile)};
    return {capability(profile), availability(profile), read(profile)};
}
Query ExtendedParameters::codec() { return read_query(table1_type, 0x12, 2); }
Query ExtendedParameters::confirm_connection_change() { return write_query(table1_type, {0x98, 0, 0, 1}); }
bool ExtendedParameters::legal(const ExtendedProfile &profile, Byte value) {
    switch (profile.feature) {
    case Feature::ConnectionPreference:
    case Feature::Dsee:
    case Feature::SpeakToChat:
    case Feature::WearDetectionPause:
        return value <= 1;
    case Feature::AutoPowerOff:
        return value <= 4 || value == 0x11 || (profile.type == 5 && value == 0x10);
    case Feature::PowerOff:
        return value == 1;
    default:
        return false;
    }
}
Query ExtendedParameters::write(const ExtendedProfile &profile, Byte value, Byte last_selected) {
    const auto &known = profiles();
    if (std::find(known.begin(), known.end(), profile) == known.end() || !legal(profile, value))
        throw std::out_of_range("Invalid extended value");
    if (profile.feature == Feature::AutoPowerOff && !legal(profile, last_selected))
        throw std::out_of_range("Invalid previous auto-power-off value");
    if (profile.feature == Feature::PowerOff)
        return write_query(table1_type, {0x24, 3, 1});
    if (profile.feature == Feature::AutoPowerOff)
        return write_query(table1_type, {0x28, profile.type, value, last_selected});
    if (profile.feature == Feature::SpeakToChat)
        return write_query(table1_type, {0xf8, profile.type, value, 1});
    if (profile.feature == Feature::ConnectionPreference && profile.function == 0xe7)
        return write_query(table1_type, {0xe8, 5, value, 1});
    return write_query(table1_type, {static_cast<Byte>(profile.group + 8), profile.type, value});
}
Query AudioSourceParameters::enable_alerts() { return write_query(table1_type, {0x94, 0, 0}); }
Query AudioSourceParameters::confirm_multipoint_alert(Byte message) {
    if (message != 6 && message != 7)
        throw std::out_of_range("Invalid multipoint alert");
    return write_query(table1_type, {0x98, 0, message, 1});
}
Query AudioSourceParameters::general_capability(Byte slot) {
    valid_slot(slot);
    return {table1_type, {0xd0, slot, 1}, 0xd1, slot, true};
}
Query AudioSourceParameters::general_availability(Byte slot) {
    return read_query(table1_type, 0xd2, valid_slot(slot));
}
Query AudioSourceParameters::general_read(Byte slot) {
    return read_query(table1_type, 0xd6, valid_slot(slot));
}
Query AudioSourceParameters::set_multipoint(Byte slot, bool enabled) {
    return write_query(table1_type, {0xd8, valid_slot(slot), 0, static_cast<Byte>(enabled ? 0 : 1)});
}
Query AudioSourceParameters::source_capability(Byte subtype) {
    return read_query(table2_type, 0x30, valid_subtype(subtype));
}
Query AudioSourceParameters::source_availability(Byte subtype) {
    return read_query(table2_type, 0x32, valid_subtype(subtype));
}
Query AudioSourceParameters::sources(Byte subtype) {
    return read_query(table2_type, 0x36, valid_subtype(subtype));
}
Query AudioSourceParameters::switch_source(const std::string &target) {
    Bytes payload{0x3c, 1};
    const auto id = address(Bytes(target.begin(), target.end()));
    payload.insert(payload.end(), id.begin(), id.end());
    return write_query(table2_type, std::move(payload));
}
SourceCategory AudioSourceParameters::category(std::optional<int> cod) {
    if (!cod)
        return SourceCategory::Unknown;
    switch ((*cod >> 8) & 31) {
    case 1:
        return SourceCategory::Computer;
    case 2:
        return SourceCategory::Phone;
    case 0:
    case 31:
        return SourceCategory::Unknown;
    default:
        return SourceCategory::Other;
    }
}
bool InteractionParameters::known_voice_assistant(Byte value) {
    return (value >= 0x30 && value <= 0x34) || value == 0xff;
}
bool InteractionParameters::known_preset(Byte value) {
    return one_of(value, {0, 0x10, 0x20, 0x21, 0x22, 0x70, 0x71, 0x72, 0xff}) ||
           (value >= 0x30 && value <= 0x39) || (value >= 0x40 && value <= 0x47);
}
bool InteractionParameters::preset_allowed(Byte value, Byte limitation) {
    return known_preset(value) && limitation <= 2 &&
           (limitation == 0 || !(value == 0x22 || (value >= 0x30 && value <= 0x34) ||
                                 (value >= 0x37 && value <= 0x39) || (value >= 0x40 && value <= 0x44)));
}
Query InteractionParameters::voice_assistant_read() { return read_query(table1_type, 0xf6, 4); }
Query InteractionParameters::voice_assistant_write(Byte value) {
    if (!known_voice_assistant(value))
        throw std::out_of_range("Invalid voice assistant");
    return write_query(table1_type, {0xf8, 4, value});
}
Query InteractionParameters::touch_read(Byte type) { return read_query(table1_type, 0xf6, type); }
Query InteractionParameters::touch_details_read(Byte type) { return read_query(table1_type, 0xfa, type); }
Query InteractionParameters::touch_write(Byte type, const Bytes &values) {
    if ((type != 3 && type != 0x0e) || values.empty() || values.size() > 6)
        throw std::out_of_range("Invalid touch assignments");
    Bytes payload{0xf8, type, static_cast<Byte>(values.size())};
    payload.insert(payload.end(), values.begin(), values.end());
    return write_query(table1_type, std::move(payload));
}
Query InteractionParameters::touch_details_write(Byte type, const std::vector<TouchPresetState> &values) {
    if ((type != 3 && type != 0x0e) || values.empty() || values.size() > 32)
        throw std::out_of_range("Invalid touch details");
    std::set<Byte> ids;
    Bytes payload{0xfc, type, static_cast<Byte>(values.size())};
    for (const auto &value : values) {
        if (value.actions.size() > 32 || !ids.insert(value.preset).second)
            throw std::out_of_range("Invalid touch details");
        payload.push_back(value.preset);
        payload.push_back(static_cast<Byte>(value.actions.size()));
        for (const auto &action : value.actions) {
            payload.push_back(action.action);
            payload.push_back(action.function);
        }
    }
    return write_query(table1_type, std::move(payload));
}
Query InteractionParameters::reply_touch_voice_assistant_alert(bool accept) {
    return write_query(table1_type, {0x98, 0, 0x0f, static_cast<Byte>(accept ? 1 : 0)});
}
Query InteractionParameters::reply_restore_voice_assistant_alert() {
    return write_query(table1_type, {0x98, 0, 0x13, 1});
}
Query InteractionParameters::cycle_read() { return read_query(table1_type, 0x66, 0x30); }
Query InteractionParameters::cycle_write(Byte value) {
    if (value < 1 || value > 4)
        throw std::out_of_range("Invalid ambient cycle");
    return write_query(table1_type, {0x68, 0x30, value});
}
Query InteractionParameters::guidance_status(Byte type) {
    return type <= 2 ? Query{table2_type, {0x42, type, 0}, 0x43, type, true}
                     : read_query(table2_type, 0x42, type);
}
Query InteractionParameters::guidance_write(Byte type, Byte value, std::optional<Byte> language) {
    if (type > 3 || value > 1 ||
        ((type == 1 || type == 2) && (!language || *language == 0 || *language == 0xff)))
        throw std::out_of_range("Invalid guidance setting");
    Bytes payload{0x48, type, value};
    if (type == 1 || type == 2)
        payload.push_back(*language);
    return write_query(table2_type, std::move(payload));
}
Query InteractionParameters::volume_read() { return read_query(table2_type, 0x46, 0x20); }
Query InteractionParameters::volume_write(int value) {
    if (value < -2 || value > 2)
        throw std::out_of_range("Invalid prompt volume");
    return write_query(table2_type, {0x48, 0x20, static_cast<Byte>(value), 0});
}
Query InteractionParameters::fix_read() { return read_query(table2_type, 0x36, 1); }
Query InteractionParameters::fix_write(bool fixed) {
    return write_query(table2_type, {0x38, 1, static_cast<Byte>(fixed ? 0 : 1)});
}

std::optional<Byte> SafeListeningParameters::select_type(const SupportFunctions &functions) {
    if (functions.table != Table::Table2)
        return {};
    std::optional<Byte> selected;
    for (const auto &f : functions.functions)
        if (f.id >= 0x50 && f.id <= 0x53) {
            const auto type = static_cast<Byte>(f.id - 0x50);
            if (selected && *selected != type)
                return {}; // Do not guess between multiple advertised layouts.
            selected = type;
        }
    return selected;
}
Query SafeListeningParameters::capability(Byte type) {
    if (type > 3) throw std::invalid_argument("Unsupported Safe Listening type");
    return read_query(table2_type, 0x50, type);
}
Query SafeListeningParameters::availability(Byte type) {
    if (type > 3) throw std::invalid_argument("Unsupported Safe Listening type");
    return read_query(table2_type, 0x56, type);
}
Query SafeListeningParameters::current(Byte type) {
    if (type > 3) throw std::invalid_argument("Unsupported Safe Listening type");
    return read_query(table2_type, 0x5a, type);
}
Query SafeListeningParameters::set(Byte type, bool enabled) {
    if (type > 3) throw std::invalid_argument("Unsupported Safe Listening type");
    // Explicit normal mode; preview is off. NTFY_PARAM confirms both fields.
    return write_query(table2_type, {0x58, type, static_cast<Byte>(enabled ? 0 : 1), 1});
}

ParseResult parse(const Frame &frame, Family family) {
    if (frame.type == ack_type) {
        if (frame.sequence > 1 || !frame.payload.empty())
            return unknown(frame, ParseError::InvalidAcknowledgement);
        return {{frame, Kind::Acknowledgement, {}, {}}, {}};
    }
    if (!frame.requires_acknowledgement())
        return unknown(frame);
    if (frame.sequence > 1)
        return unknown(frame, ParseError::InvalidSequence);
    if (frame.payload.empty())
        return unknown(frame, ParseError::EmptyPayload);
    std::span<const Byte> p = frame.payload;
    const auto command = p[0];
    if (frame.type == table1_type && command == 1) {
        if (p.size() < 2 || p[1] != 0)
            return malformed(frame);
        if (p.size() == 4)
            return result(frame, Kind::Response,
                          ProtocolInformation{Family::V1, big_endian(p.subspan(2)), true, {}});
        if (p.size() != 8 || p[6] > 1 || p[7] > 1)
            return malformed(frame);
        return result(frame, Kind::Response,
                      ProtocolInformation{Family::V2, big_endian(p.subspan(2, 4)), p[6] == 0, p[7] == 0});
    }
    if (family != Family::V2)
        return unknown(frame, ParseError::ProtocolNotNegotiated);
    try {
        if (auto value = parse_safe_listening(frame))
            return *value;
        if (auto value = parse_interaction(frame))
            return *value;
        if (auto value = parse_sound(frame))
            return *value;
        if (auto value = parse_equalizer(frame))
            return *value;
        if (auto value = parse_sources(frame))
            return *value;
        if (auto value = parse_extended(frame))
            return *value;
        if (auto value = parse_battery(frame))
            return *value;
        if (command == 7) {
            if (p.size() < 3 || p[1] != 0 || p.size() != 3u + 2u * p[2])
                return malformed(frame);
            SupportFunctions value{frame.type == table1_type ? Table::Table1 : Table::Table2, {}};
            for (std::size_t i = 3; i < p.size(); i += 2)
                value.functions.push_back({p[i], p[i + 1]});
            return result(frame, Kind::Response, value);
        }
        if (frame.type == table1_type && command == 3) {
            if (p.size() < 4 || p[1] != 0 || p.size() != 4u + p[3])
                return malformed(frame);
            return result(frame, Kind::Response, CapabilityInformation{p[2], tail(p, 4)});
        }
        if (frame.type == table1_type && command == 5) {
            if (p.size() < 2)
                return malformed(frame);
            if (p[1] != 1 && p[1] != 2)
                return unknown(frame);
            if (p.size() < 3 || p.size() != 3u + p[2] || p[2] == 0)
                return malformed(frame);
            return result(frame, Kind::Response,
                          DeviceInformation{static_cast<DeviceInfoField>(p[1]), utf8(p.subspan(3))});
        }
    } catch (const std::invalid_argument &) {
        return malformed(frame);
    }
    const auto kind = classify(frame.type, command);
    if (kind != Kind::Unknown && p.size() < 2)
        return malformed(frame);
    return result(frame, kind);
}
Handshake::Handshake(std::uint64_t session) {
    if (session == 0)
        throw std::invalid_argument("A connection session is required");
    state_.session = session;
}
std::optional<Query> Handshake::current_query() const {
    switch (state_.phase) {
    case Phase::Protocol:
        return protocol_info();
    case Phase::CapabilityInfo:
        return capability_info();
    case Phase::ModelName:
        return model_name();
    case Phase::Firmware:
        return firmware();
    case Phase::Table1Functions:
        return functions(Table::Table1);
    case Phase::Table2Functions:
        return functions(Table::Table2);
    default:
        return {};
    }
}
HandshakeStep Handshake::receive(std::uint64_t session, const Frame &frame) {
    if (session != state_.session)
        return {false, {}, "Response belongs to another session"};
    auto parsed = parse(frame, state_.protocol ? state_.protocol->family : Family::Unknown);
    if (!parsed.valid())
        return {false, parsed, "Malformed or premature message; query remains pending"};
    const auto query = current_query();
    if (!query || parsed.message.kind != Kind::Response || frame.type != query->type ||
        parsed.message.command != query->response_command || frame.payload.size() < 2 ||
        frame.payload[1] != query->inquired_type)
        return {false, parsed, "Unrelated message; query remains pending"};
    const auto &info = parsed.message.information;
    bool applied = false;
    if (state_.phase == Phase::Protocol) {
        if (auto value = std::get_if<ProtocolInformation>(&info)) {
            state_.protocol = *value;
            applied = true;
            if (value->family != Family::V2) {
                state_.phase = Phase::Unsupported;
                state_.error = "MDR V1 recognized; this bootstrap targets V2";
            } else if (!value->table1) {
                state_.phase = Phase::Unsupported;
                state_.error = "MDR V2 Table 1 is disabled";
            } else
                state_.phase = Phase::CapabilityInfo;
        }
    } else if (state_.phase == Phase::CapabilityInfo) {
        if (auto value = std::get_if<CapabilityInformation>(&info)) {
            state_.capability = *value;
            state_.phase = Phase::ModelName;
            applied = true;
        }
    } else if (state_.phase == Phase::ModelName || state_.phase == Phase::Firmware) {
        if (auto value = std::get_if<DeviceInformation>(&info)) {
            if (state_.phase == Phase::ModelName && value->field == DeviceInfoField::ModelName) {
                state_.model_name = value->value;
                state_.phase = Phase::Firmware;
                applied = true;
            } else if (state_.phase == Phase::Firmware && value->field == DeviceInfoField::Firmware) {
                state_.firmware = value->value;
                state_.phase = Phase::Table1Functions;
                applied = true;
            }
        }
    } else if (auto value = std::get_if<SupportFunctions>(&info)) {
        if (state_.phase == Phase::Table1Functions && value->table == Table::Table1) {
            state_.table1_functions = *value;
            state_.phase = state_.protocol->table2.value_or(false) ? Phase::Table2Functions : Phase::Ready;
            applied = true;
        } else if (state_.phase == Phase::Table2Functions && value->table == Table::Table2) {
            state_.table2_functions = *value;
            state_.phase = Phase::Ready;
            applied = true;
        }
    }
    return {applied, parsed, applied ? "" : "Response information does not match pending query"};
}
bool Capabilities::readable(Feature feature) const {
    const auto found = features.find(feature);
    return found != features.end() && found->second.access.read == Support::Supported;
}
Capabilities map_capabilities(const HandshakeState &state) {
    Capabilities output;
    if (!state.protocol || state.protocol->family != Family::V2 || state.phase == Phase::Unsupported)
        return output;
    auto reported = [](bool available, std::string reason) {
        return Capability{
            available ? FeatureAccess{Support::Supported, Support::Unknown, Support::Unknown}
                      : FeatureAccess{Support::Unsupported, Support::Unsupported, Support::Unsupported},
            std::move(reason), false};
    };
    if (state.firmware)
        output.features[Feature::Firmware] = reported(true, "CONNECT_RET_DEVICE_INFO / firmware");
    const std::vector<std::pair<Feature, Bytes>> table1{
        {Feature::SingleBattery, {0x20, 0x28}},
        {Feature::LeftBattery, {0x21, 0x29}},
        {Feature::RightBattery, {0x21, 0x29}},
        {Feature::CaseBattery, {0x22, 0x2a}},
        {Feature::SoundMode, {0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x6a, 0x6b, 0x6c, 0x6d}},
        {Feature::AmbientLevel, {0x64, 0x65, 0x67, 0x68, 0x6a, 0x6b, 0x6c, 0x6d}},
        {Feature::Equalizer, {0x50, 0x52, 0x53, 0x55, 0x57}},
        {Feature::NegotiatedCodec, {0x12}},
        {Feature::Dsee, {0xe2, 0xed}},
        {Feature::ConnectionPreference, {0xe1, 0xe3}},
        {Feature::SpeakToChat, {0xf2, 0xfc}},
        {Feature::WearDetectionPause, {0xf1}},
        {Feature::AutoPowerOff, {0x24, 0x25}},
        {Feature::TouchAssignment, {0xf3, 0xfe}},
        {Feature::PowerOff, {0x23}}};
    const std::vector<std::pair<Feature, Bytes>> table2{{Feature::ConnectedSources, {0x30, 0x32, 0x33}},
                                                        {Feature::SourceSwitch, {0x31}},
                                                        {Feature::FixedPlayback, {0x31}},
                                                        {Feature::PromptVolume, {0x42, 0x43}},
                                                        {Feature::SoundPressure, {0x50, 0x51, 0x52, 0x53}}};
    auto map = [&](const SupportFunctions &functions, const auto &mappings, const char *source) {
        for (const auto &[feature, ids] : mappings) {
            const bool advertised =
                std::any_of(ids.begin(), ids.end(), [&](Byte id) { return functions.contains(id); });
            output.features[feature] = reported(
                advertised,
                std::string(source) +
                    (advertised ? ": advertised; write/notify require operation-specific confirmation"
                                : ": no mapped function advertised"));
        }
    };
    if (state.table1_functions)
        map(*state.table1_functions, table1, "V2 Table 1");
    if (state.table2_functions)
        map(*state.table2_functions, table2, "V2 Table 2");
    else if (state.protocol->table2 == false)
        for (const auto &[feature, ids] : table2)
            output.features[feature] = reported(false, "Protocol explicitly disables V2 Table 2");
    output.complete = state.phase == Phase::Ready;
    return output;
}

} // namespace sonyconnect::protocol
