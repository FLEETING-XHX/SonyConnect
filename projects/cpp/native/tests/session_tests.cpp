#include "sonyconnect/session.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace sonyconnect;
using namespace sonyconnect::protocol;
namespace {
void check(bool condition, const char *message = "Session expectation failed") {
    if (!condition)
        throw std::runtime_error(message);
}
DeviceDescriptor device() { return {0x112233445566, "WF-1000XM5", "BTH:112233445566", true, true, true}; }
SessionPolicy policy() {
    SessionPolicy p;
    p.request_timeout = Milliseconds{60};
    p.connect_timeout = Milliseconds{60};
    p.retry_delay = Milliseconds{1};
    p.maximum_connect_attempts = p.maximum_query_attempts = 2;
    return p;
}
class FakeTransport final : public ByteTransport {
  public:
    std::function<void(FakeTransport &, const Frame &)> handler;
    std::function<void(const DeviceDescriptor &)> before_open;
    std::atomic<unsigned> opens{}, closes{}, requests{}, acknowledgements{};
    std::atomic<std::uint64_t> last_open_address{};
    std::vector<Frame> writes;
    std::atomic<unsigned> fail_opens{};
    Result open(const DeviceDescriptor &device, Milliseconds) override {
        ++opens;
        last_open_address = device.address;
        if (before_open)
            before_open(device);
        if (fail_opens) {
            --fail_opens;
            return {ResultCode::Transport, "Injected connection failure"};
        }
        std::lock_guard lock(gate);
        online = true;
        inbound.clear();
        return {};
    }
    ReadResult read(std::span<Byte> bytes, Milliseconds timeout) override {
        std::unique_lock lock(gate);
        cv.wait_for(lock, timeout, [&] { return !online || !inbound.empty(); });
        if (!online)
            return {ReadStatus::Closed};
        if (inbound.empty())
            return {ReadStatus::Timeout};
        const auto count = std::min(bytes.size(), inbound.size());
        for (std::size_t i = 0; i < count; ++i) {
            bytes[i] = inbound.front();
            inbound.pop_front();
        }
        return {ReadStatus::Data, count};
    }
    void write(std::span<const Byte> bytes, Milliseconds) override {
        const auto frame = unpack(bytes);
        check(frame.success(), "Session wrote a corrupt frame");
        {
            std::lock_guard lock(gate);
            if (!online)
                throw std::runtime_error("Fake disconnected");
            writes.push_back(*frame.frame);
        }
        if (frame.frame->type == ack_type) {
            ++acknowledgements;
            return;
        }
        ++requests;
        if (handler)
            handler(*this, *frame.frame);
    }
    void close() noexcept override {
        std::lock_guard lock(gate);
        online = false;
        ++closes;
        cv.notify_all();
    }
    void push(const Frame &frame) {
        const auto wire = pack(frame);
        std::lock_guard lock(gate);
        inbound.insert(inbound.end(), wire.begin(), wire.end());
        cv.notify_all();
    }
    void reply(const Frame &request, Bytes payload) {
        push(acknowledgement_for(request));
        push({request.type, incoming_sequence, std::move(payload)});
        incoming_sequence = static_cast<Byte>(1 - incoming_sequence);
    }
    void acknowledge(const Frame &request) { push(acknowledgement_for(request)); }

  private:
    std::mutex gate;
    std::condition_variable cv;
    std::deque<Byte> inbound;
    bool online{};
    Byte incoming_sequence{};
};
Bytes protocol_version(bool table2 = false) {
    return {1, 0, 3, 0, 0x30, 0x18, 0, static_cast<Byte>(table2 ? 0 : 1)};
}
void prime(ProtocolSession &session, FakeTransport &fake) {
    fake.handler = [](auto &transport, const auto &frame) { transport.reply(frame, protocol_version()); };
    check(session.connect(device()).ok());
    check(session.exchange(protocol_info()).result.confirmed());
}
template <class F> void eventually(F predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(Milliseconds{1});
    check(predicate(), "Expected asynchronous condition did not occur");
}
struct Headphones {
    Bytes functions1, functions2;
    std::string model = "WF-1000XM5";
    SoundParameter sound{true, true, 1, 0, 7};
    Byte eq_preset = 0x10;
    Bytes eq_steps{10, 10, 10, 10, 10, 10};
    Bytes eq_presets{0, 0x10, 0xa0, 0xa1, 0xa2}, last_eq_write;
    Byte preference{}, switching{}, guidance{}, prompt_volume{}, right_assignment{0x10}, assistant{0x30};
    Byte guidance_type{3}, guidance_switch{1}, guidance_language{0xf0};
    bool guidance_available{true};
    unsigned guidance_writes{};
    Bytes last_guidance_write;
    Byte safe_interval{1}, safe_level{72}, safe_cause{3};
    bool safe_available{true}, safe_enabled{}, safe_preview{}, safe_confirm{true}, safe_accept{true};
    unsigned safe_reads{}, safe_writes{};
    std::function<void()> during_safe_read;
    Byte key_status{}, limitation{};
    Byte playback_slot{1};
    bool multipoint{true}, fixed{}, custom_touch{}, ordinary_cycle{}, reject_source{};
    Byte cycle_value{1}, detail_function{1};
    unsigned source_writes{}, fix_writes{}, detail_writes{};
    const std::string first_source = "AA:BB:CC:DD:EE:01", second_source = "AA:BB:CC:DD:EE:02";
    bool accept_sound = true, accept_eq = true, accept_extended = true;
    std::atomic<unsigned> sound_writes{};
    unsigned eq_writes{}, extended_writes{}, touch_writes{}, assistant_writes{};
    Bytes functions(Byte command, const Bytes &list) {
        Bytes p{command, 0, static_cast<Byte>(list.size())};
        for (auto id : list) {
            p.push_back(id);
            p.push_back(0xff);
        }
        return p;
    }
    void operator()(FakeTransport &transport, const Frame &request) {
        const auto &p = request.payload;
        const auto command = p[0], type = p[1];
        if (command == 0) {
            transport.reply(request, protocol_version(!functions2.empty()));
            return;
        }
        if (command == 2) {
            transport.reply(request, {3, 0, 1, 0});
            return;
        }
        if (command == 4) {
            const auto value = type == 1 ? model : "6.1.0";
            Bytes response{5, type, static_cast<Byte>(value.size())};
            response.insert(response.end(), value.begin(), value.end());
            transport.reply(request, response);
            return;
        }
        if (command == 6) {
            transport.reply(request, functions(7, request.type == table1_type ? functions1 : functions2));
            return;
        }
        if (request.type == table1_type) {
            if (command == 0x60 && type == 0x17) {
                transport.reply(request, {0x61, 0x17, 2, 0, 0, 20, 1, 1, 0, 20, 1});
                return;
            }
            if (command == 0x62 && type == 0x17) {
                transport.reply(request, {0x63, 0x17, 0});
                return;
            }
            if (command == 0x66 && type == 0x17) {
                transport.reply(request, {0x67, 0x17, static_cast<Byte>(sound.settled),
                                          static_cast<Byte>(sound.enabled), sound.mode, sound.ambient_mode,
                                          sound.level});
                return;
            }
            if (command == 0x68 && type == 0x17) {
                ++sound_writes;
                if (accept_sound)
                    sound = {true, p[3] == 1, p[4], p[5], p[6]};
                transport.acknowledge(request);
                return;
            }
            if (command == 0x50) {
                Bytes response{0x51, 0, 6, 21, static_cast<Byte>(eq_presets.size())};
                for (const auto preset : eq_presets) {
                    response.push_back(preset);
                    response.push_back(0);
                }
                transport.reply(request, response);
                return;
            }
            if (command == 0x52) {
                transport.reply(request, {0x53, 0, 0});
                return;
            }
            if (command == 0x56) {
                Bytes response{0x57, 0, eq_preset, static_cast<Byte>(eq_steps.size())};
                response.insert(response.end(), eq_steps.begin(), eq_steps.end());
                transport.reply(request, response);
                return;
            }
            if (command == 0x58) {
                ++eq_writes;
                last_eq_write = p;
                if (accept_eq) {
                    eq_preset = p[2];
                    if (p[3])
                        eq_steps = Bytes(p.begin() + 4, p.end());
                }
                transport.acknowledge(request);
                return;
            }
            if (command == 0xe0 && type == 5) {
                transport.reply(request, {0xe1, 5, 3, 0, 1, 2, 1, 0});
                return;
            }
            if (command == 0xe2 && type == 5) {
                transport.reply(request, {0xe3, 5, 0, 0});
                return;
            }
            if (command == 0xe6 && type == 5) {
                transport.reply(request, {0xe7, 5, preference, switching});
                return;
            }
            if (command == 0xe8 && type == 5) {
                ++extended_writes;
                if (accept_extended)
                    preference = p[2];
                transport.acknowledge(request);
                return;
            }
            if (command == 0xd0) {
                const std::string name = "MULTIPOINT_SETTING";
                Bytes response{0xd1, type, 0, 1, static_cast<Byte>(name.size())};
                response.insert(response.end(), name.begin(), name.end());
                response.push_back(0);
                transport.reply(request, response);
                return;
            }
            if (command == 0xd2) {
                transport.reply(request, {0xd3, type, 0});
                return;
            }
            if (command == 0xd6) {
                transport.reply(request, {0xd7, type, 0, static_cast<Byte>(multipoint ? 0 : 1)});
                return;
            }
            if (command == 0xd8) {
                multipoint = p[3] == 0;
                transport.acknowledge(request);
                return;
            }
            if (command == 0x12 && type == 2) {
                transport.reply(request, {0x13, 2, 2});
                return;
            }
            if (command == 0xf0 && type == 3) {
                if (custom_touch)
                    transport.reply(request,
                                    {0xf1, 3, 1, 0, 0, 0x70, 1, 0x70, 1, 1, 1, 0x20, 0, 1, 4, 1, 2, 3, 4});
                else if (ordinary_cycle)
                    transport.reply(request, {0xf1, 3, 1, 0, 0, 0x70, 1, 0x70, 1, 0, 0, 1});
                else
                    transport.reply(request, {0xf1, 3, 1, 1, 0, 0x10, 2, 0x10, 0, 0, 0xff, 0, 0});
                return;
            }
            if (command == 0xf2 && type == 3) {
                transport.reply(request, {0xf3, 3, 1, key_status});
                return;
            }
            if (command == 0xf6 && type == 3) {
                transport.reply(request,
                                {0xf7, 3, 1, custom_touch || ordinary_cycle ? Byte{0x70} : right_assignment});
                return;
            }
            if (command == 0xfa && type == 3) {
                transport.reply(request, {0xfb, 3, 1, 0x70, 2, 1, 0x20, 0, detail_function});
                return;
            }
            if (command == 0xfc && type == 3) {
                ++detail_writes;
                detail_function = p.back();
                transport.acknowledge(request);
                return;
            }
            if (command == 0x60 && type == 0x30) {
                transport.reply(request, {0x61, 0x30, 0});
                return;
            }
            if (command == 0x62 && type == 0x30) {
                transport.reply(request, {0x63, 0x30, 0});
                return;
            }
            if (command == 0x66 && type == 0x30) {
                transport.reply(request, {0x67, 0x30, cycle_value});
                return;
            }
            if (command == 0x68 && type == 0x30) {
                cycle_value = p[2];
                transport.acknowledge(request);
                return;
            }
            if (command == 0xf0 && type == 4) {
                transport.reply(request, {0xf1, 4, 3, 2, 0x30, 0xff});
                return;
            }
            if (command == 0xf6 && type == 4) {
                transport.reply(request, {0xf7, 4, assistant});
                return;
            }
            if (command == 0xf8 && type == 3) {
                ++touch_writes;
                right_assignment = p[3];
                transport.acknowledge(request);
                if (right_assignment == 0xff && assistant != 0xff) {
                    assistant = 0xff;
                    transport.push({table1_type, 0, {0x99, 0, 0x0f, 1}});
                }
                return;
            }
            if (command == 0xf8 && type == 4) {
                ++assistant_writes;
                assistant = p[2];
                transport.acknowledge(request);
                transport.push({table1_type, 1, {0x99, 0, 0x13, 1}});
                return;
            }
            if (command == 0x98 || command == 0x94) {
                transport.acknowledge(request);
                return;
            }
        } else {
            if (command == 0x50 && type <= 3) {
                transport.reply(request, {0x51, type, 1, 0, 0, 0, 0, safe_interval, 10});
                return;
            }
            if (command == 0x56 && type <= 3) {
                transport.reply(request, {0x57, type, static_cast<Byte>(safe_available ? 0 : 1)});
                return;
            }
            if (command == 0x58 && type <= 3) {
                ++safe_writes;
                if (safe_accept) {
                    safe_enabled = p[2] == 0;
                    safe_preview = p[3] == 0;
                }
                if (safe_confirm)
                    transport.reply(request, {0x59, type, static_cast<Byte>(safe_enabled ? 0 : 1),
                                              static_cast<Byte>(safe_preview ? 0 : 1)});
                else
                    transport.acknowledge(request);
                return;
            }
            if (command == 0x5a && type <= 3) {
                ++safe_reads;
                if (during_safe_read) during_safe_read();
                transport.reply(request, {0x5b, type, safe_level, safe_cause});
                return;
            }
            if (command == 0x30) {
                transport.reply(request, {0x31, type, 2, 2, 0});
                return;
            }
            if (command == 0x32) {
                transport.reply(request, {0x33, type, 0, 0});
                return;
            }
            if (command == 0x36 && type == 0) {
                Bytes response{0x37, 0, 2};
                response.insert(response.end(), first_source.begin(), first_source.end());
                response.insert(response.end(), {1, 1, 'A'});
                response.insert(response.end(), second_source.begin(), second_source.end());
                response.insert(response.end(), {2, 1, 'B', playback_slot});
                transport.reply(request, response);
                return;
            }
            if (command == 0x3c) {
                ++source_writes;
                const std::string target(p.begin() + 2, p.end());
                if (!reject_source)
                    playback_slot = target == first_source ? 1 : 2;
                transport.acknowledge(request);
                Bytes response{0x3d, 1, static_cast<Byte>(reject_source ? 1 : 0)};
                response.insert(response.end(), target.begin(), target.end());
                transport.push({table2_type, 0, response});
                return;
            }
            if (command == 0x36 && type == 1) {
                transport.reply(request, {0x37, 1, static_cast<Byte>(fixed ? 0 : 1)});
                return;
            }
            if (command == 0x38 && type == 1) {
                ++fix_writes;
                fixed = p[2] == 0;
                transport.acknowledge(request);
                return;
            }
            if (command == 0x40 && type == guidance_type && type <= 2) {
                Bytes capability = type <= 1 ? Bytes{0x41, type, 0, 0, 0, 0, guidance_switch}
                                             : Bytes{0x41, type, guidance_switch};
                if (type == 1 || type == 2)
                    capability.insert(capability.end(), {1, 0xf0});
                transport.reply(request, capability);
                return;
            }
            if (command == 0x42 && type == guidance_type) {
                Bytes status{0x43, type};
                if (type <= 2)
                    status.push_back(0);
                status.push_back(guidance_available ? 0 : 1);
                transport.reply(request, status);
                return;
            }
            if (command == 0x46 && type == guidance_type) {
                Bytes value{0x47, type, guidance};
                if (type == 1 || type == 2)
                    value.push_back(guidance_language);
                transport.reply(request, value);
                return;
            }
            if (command == 0x46 && type == 0x20) {
                transport.reply(request, {0x47, 0x20, prompt_volume});
                return;
            }
            if (command == 0x48 && type == guidance_type) {
                ++guidance_writes;
                last_guidance_write = p;
                guidance = p[2];
                if (type == 1 || type == 2) {
                    check(p.size() == 4 && p[3] == guidance_language,
                          "Guidance switch must preserve the device's current language");
                } else
                    check(p.size() == 3, "On/off-only guidance payload must omit language");
                transport.acknowledge(request);
                return;
            }
            if (command == 0x48 && type == 0x20) {
                prompt_volume = p[2];
                transport.acknowledge(request);
                return;
            }
        }
        throw std::runtime_error("Unexpected fake device query " + std::to_string(command) + ":" +
                                 std::to_string(type));
    }
};
std::unique_ptr<FakeTransport> fake_for(Headphones &model) {
    auto fake = std::make_unique<FakeTransport>();
    fake->handler = [&](auto &t, const auto &f) { model(t, f); };
    return fake;
}
Bytes source_notification(const Headphones &model, Byte playback_slot) {
    Bytes response{0x39, 0, 2};
    response.insert(response.end(), model.first_source.begin(), model.first_source.end());
    response.insert(response.end(), {1, 1, 'A'});
    response.insert(response.end(), model.second_source.begin(), model.second_source.end());
    response.insert(response.end(), {2, 1, 'B', playback_slot});
    return response;
}
} // namespace
int main(int argc, char **argv) {
    std::string filter;
    if (argc > 2 || (argc == 2 && !std::string(argv[1]).starts_with("--filter="))) return 2;
    if (argc == 2) filter = std::string(argv[1]).substr(9);
    const std::vector<std::pair<const char *, std::function<void()>>> tests{
        {"trace-diagnostics-timeout-contains-request-summary-and-failure-reason", [] {
             std::mutex trace_gate;
             std::vector<ProtocolTrace> traces;
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             session.set_trace([&](const auto &trace) {
                 std::lock_guard lock(trace_gate);
                 traces.push_back(trace);
             });
             transport->handler = [](auto &, const auto &) {};
             check(session.exchange(SafeListeningParameters::current(1)).result.code == ResultCode::Timeout);
             check(!session.connected(), "Timeout must retain the existing connection quarantine");
             session.disconnect();
             const auto timeout = std::find_if(traces.begin(), traces.end(), [](const auto &trace) {
                 return trace.event == "query-timeout";
             });
             check(timeout != traces.end() && timeout->type == table2_type && timeout->command == 0x5a &&
                   timeout->sequence.has_value(), "Timeout must identify the existing pressure query without payload");
             check(std::any_of(traces.begin(), traces.end(), [](const auto &trace) {
                 return trace.event == "link-failed" && trace.failure_reason.find("timeout") != std::string::npos;
             }), "Link failure must retain the timeout reason");
         }},
        {"trace-diagnostics-peer-close-preserves-transport-reason", [] {
             std::mutex trace_gate;
             std::vector<ProtocolTrace> traces;
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             session.set_trace([&](const auto &trace) {
                 std::lock_guard lock(trace_gate);
                 traces.push_back(trace);
             });
             transport->close();
             eventually([&] {
                 std::lock_guard lock(trace_gate);
                 return std::any_of(traces.begin(), traces.end(), [](const auto &trace) {
                     return trace.event == "link-failed" &&
                            trace.failure_reason == "Headphones closed the RFCOMM control channel.";
                 });
             });
             session.disconnect();
             check(std::none_of(traces.begin(), traces.end(), [](const auto &trace) {
                 return trace.event == "query-timeout";
             }), "A peer closure must not be logged as a request timeout");
         }},
        {"trace-diagnostics-success-keeps-normal-traffic-and-empty-reason", [] {
             std::mutex trace_gate;
             std::vector<ProtocolTrace> traces;
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             session.set_trace([&](const auto &trace) {
                 std::lock_guard lock(trace_gate);
                 traces.push_back(trace);
             });
             transport->handler = [](auto &t, const auto &frame) {
                 t.reply(frame, {0x67, 0x17, 1, 1, 0, 0, 7});
             };
             check(session.exchange(SoundParameters::read()).result.confirmed());
             session.set_trace({});
             session.disconnect();
             check(std::any_of(traces.begin(), traces.end(), [](const auto &trace) { return trace.event == "send"; }));
             check(std::all_of(traces.begin(), traces.end(), [](const auto &trace) {
                 return trace.failure_reason.empty() && trace.event != "query-timeout" && trace.event != "link-failed";
             }), "Successful traffic must not acquire a failure reason");
         }},
        {"safe-listening-confirmation-poll-throttle-and-disable", [] {
             Headphones headphones;
             headphones.functions2 = {0x53};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             auto snap = controller.snapshot();
             check(snap.safe_listening.writable && !snap.safe_listening.enabled.known());
             check(headphones.safe_writes == 0 && headphones.safe_reads == 0);
             check(!controller.poll_sound_pressure().ok());
             check(controller.set_safe_listening(true).confirmed());
             headphones.during_safe_read = [&] { check(!controller.snapshot().busy); };
             check(controller.poll_sound_pressure().ok());
             snap = controller.snapshot();
             check(snap.safe_listening.decibels.known() && snap.safe_listening.decibels.value == 72);
             check(snap.safe_listening.updated_at.has_value());
             const auto prior = snap.last_result.code;
             check(controller.poll_sound_pressure().ok() && headphones.safe_reads == 1);
             check(controller.snapshot().last_result.code == prior);
             check(controller.set_safe_listening(false).confirmed());
             check(!controller.snapshot().safe_listening.decibels.known());
             check(!controller.poll_sound_pressure().ok() && headphones.safe_reads == 1);
             check(headphones.safe_writes == 2);
         }},
        {"safe-listening-ack-does-not-confirm-mode-or-display-values", [] {
             Headphones headphones;
             headphones.functions2 = {0x53};
             headphones.safe_confirm = false;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_safe_listening(true).code == ResultCode::Timeout);
             check(!controller.snapshot().safe_listening.enabled.known());
             check(!controller.poll_sound_pressure().ok() && headphones.safe_reads == 0);
         }},
        {"safe-listening-rejection-unavailability-and-zero-interval", [] {
             for (int condition = 0; condition < 3; ++condition) {
                 Headphones headphones;
                 headphones.functions2 = {0x53};
                 headphones.safe_accept = false;
                 if (condition == 1) headphones.safe_available = false;
                 if (condition == 2) headphones.safe_interval = 0;
                 DeviceController controller(fake_for(headphones), policy());
                 check(controller.connect(device()).ok());
                 check(controller.set_safe_listening(true).code ==
                       (condition == 0 ? ResultCode::DeviceRejected : ResultCode::Unavailable));
                 check(headphones.safe_writes == (condition == 0 ? 1u : 0u));
                 check(!controller.poll_sound_pressure().ok());
             }
         }},
        {"safe-listening-special-states-and-disconnection-never-become-decibels", [] {
             for (auto cause : {0, 1, 2}) {
                 Headphones headphones;
                 headphones.functions2 = {0x51};
                 headphones.safe_level = cause == 0 ? Byte{0xff} : Byte{72};
                 headphones.safe_cause = static_cast<Byte>(cause);
                 DeviceController controller(fake_for(headphones), policy());
                 check(controller.connect(device()).ok());
                 check(controller.snapshot().safe_listening.type == 1);
                 check(controller.set_safe_listening(true).confirmed());
                 check(controller.poll_sound_pressure().ok());
                 check(!controller.snapshot().safe_listening.decibels.known());
                 check(controller.snapshot().safe_listening.cause.value == cause);
                 controller.disconnect();
                 check(!controller.snapshot().safe_listening.writable);
                 check(!controller.snapshot().safe_listening.enabled.known());
             }
         }},
        {"discovery-excludes-paired-disconnected-headphones",
         [] {
             auto wf = device();
             auto wh = wf;
             wh.name = "WH-1000XM5";
             wh.connected_hint = false;
             check(is_connected_sony_device(wf));
             check(!is_connected_sony_device(wh));
             wh.connected_hint = true;
             check(is_connected_sony_device(wh));
             wh.paired = false;
             check(!is_connected_sony_device(wh));
             wf.name = "Other Bluetooth device";
             wf.sony_service_hint = false;
             check(!is_connected_sony_device(wf));
             wf.sony_service_hint = true;
             check(is_connected_sony_device(wf));
             wf.connected_hint = false;
             check(!is_connected_sony_device(wf));
         }},
        {"physical-connect-retry-is-bounded",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             transport->fail_opens = 4;
             ProtocolSession session(std::move(fake), policy());
             check(session.connect(device()).code == ResultCode::Transport);
             check(transport->opens == 2);
             check(!session.connected());
         }},
        {"matching-ack-and-response-required",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             transport->handler = [](auto &t, const auto &frame) {
                 t.push({ack_type, frame.sequence, {}});
                 t.push({table1_type, 0, protocol_version()});
             };
             check(session.connect(device()).ok());
             check(session.exchange(protocol_info()).result.code == ResultCode::Timeout);
             check(!session.connected());
             check(transport->requests == 1);
         }},
        {"ack-only-write-is-delivery-never-confirmed",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             transport->handler = [](auto &t, const auto &f) { t.acknowledge(f); };
             const auto reply = session.exchange(write_query(table1_type, {0x68, 0x30, 1}));
             check(reply.result.ok());
             check(!reply.result.confirmed());
             check(reply.result.code == ResultCode::Delivered);
         }},
        {"acked-query-timeout-is-not-retried",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             transport->requests = 0;
             transport->handler = [](auto &t, const auto &f) { t.acknowledge(f); };
             check(session.exchange(SoundParameters::read()).result.code == ResultCode::Timeout);
             check(transport->requests == 1);
             check(!session.connected());
         }},
        {"unacknowledged-query-retries-same-sequence",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             transport->requests = 0;
             std::optional<Frame> first;
             transport->handler = [&](auto &t, const auto &f) {
                 if (!first)
                     first = f;
                 else {
                     check(f == *first);
                     t.reply(f, {0x67, 0x17, 1, 1, 0, 0, 7});
                 }
             };
             check(session.exchange(SoundParameters::read()).result.confirmed());
             check(transport->requests == 2);
         }},
        {"writes-never-retransmit-and-late-replies-quarantined",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             transport->requests = 0;
             transport->handler = {};
             check(session.exchange(write_query(table1_type, {0x68, 0x30, 1})).result.code ==
                   ResultCode::Timeout);
             check(transport->requests == 1);
             transport->push({ack_type, 0, {}});
             check(session.exchange(protocol_info()).result.code == ResultCode::NotConnected);
         }},
        {"notification-retransmission-acked-twice-applied-once",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             std::atomic<unsigned> observations{};
             session.set_observer([&](const Observation &item) {
                 check(item.message.kind == Kind::Notification);
                 ++observations;
             });
             const auto baseline = transport->acknowledgements.load();
             const Frame frame{table1_type, 1, {0x25, 0, 80, 0}};
             transport->push(frame);
             transport->push(frame);
             eventually([&] { return transport->acknowledgements >= baseline + 2; });
             check(observations == 1);
         }},
        {"disconnect-interrupts-pending-request-and-new-link-resets-sequence",
         [] {
             auto fake = std::make_unique<FakeTransport>();
             auto *transport = fake.get();
             ProtocolSession session(std::move(fake), policy());
             prime(session, *transport);
             transport->handler = {};
             auto request =
                 std::async(std::launch::async, [&] { return session.exchange(SoundParameters::read()); });
             eventually([&] { return transport->requests >= 2; });
             session.disconnect();
             check(request.get().result.code == ResultCode::NotConnected);
             const auto old_link = session.link();
             prime(session, *transport);
             check(session.link() > old_link);
         }},
        {"six-stage-bootstrap-known-identity-no-hardware-claim",
         [] {
             Headphones headphones;
             auto fake = fake_for(headphones);
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).confirmed());
             const auto snapshot = controller.snapshot();
             check(snapshot.ready && snapshot.model == "WF-1000XM5" && snapshot.firmware == "6.1.0");
             check(!snapshot.hardware_verified && !snapshot.last_result.hardware_verified);
         }},
        {"sound-write-confirmed-by-readback-and-range-checked",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x6b};
             auto fake = fake_for(headphones);
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             check(controller.set_sound(SoundMode::Ambient, 12, true).confirmed());
             check(controller.snapshot().sound.ambient_level.value == 12);
             check(controller.set_sound({}, 21).code == ResultCode::InvalidValue);
             check(headphones.sound_writes == 1);
         }},
        {"sound-ack-does-not-hide-device-rejection",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x6b};
             headphones.accept_sound = false;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_sound({}, 12).code == ResultCode::DeviceRejected);
             check(controller.snapshot().sound.ambient_level.value == 7);
         }},
        {"voice-focus-requires-current-ambient-mode",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x6b};
             headphones.sound.mode = 0;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_sound({}, {}, true).code == ResultCode::InvalidValue);
             check(headphones.sound_writes == 0);
         }},
        {"unreviewed-model-retains-reads-and-blocks-writes",
         [] {
             Headphones headphones;
             headphones.model = "WH-1000XM5";
             headphones.functions1 = {0x50};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.snapshot().equalizer.preset.known());
             check(!controller.snapshot().equalizer.writable);
             check(controller.set_equalizer(Byte{0xa0}).code == ResultCode::Unavailable);
             check(headphones.eq_writes == 0);
         }},
        {"equalizer-built-in-edit-becomes-manual-and-preserves-other-bands",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 2, -4, 3).confirmed());
             check(headphones.eq_preset == 0xa0 && headphones.eq_steps == Bytes({13, 10, 10, 6, 10, 10}));
         }},
        {"equalizer-custom-one-edit-preserves-user-slot-and-other-bands",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_preset = 0xa1;
             headphones.eq_steps = {7, 11, 12, 13, 14, 15};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 1, -4).confirmed());
             check(headphones.last_eq_write == Bytes({0x58, 0, 0xa1, 6, 7, 11, 6, 13, 14, 15}));
             check(controller.snapshot().equalizer.preset.value == 0xa1);
             check(controller.snapshot().equalizer.bands.value == std::vector<int>({1, -4, 3, 4, 5}));
         }},
        {"equalizer-custom-two-bass-edit-preserves-user-slot-and-other-bands",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_preset = 0xa2;
             headphones.eq_steps = {7, 11, 12, 13, 14, 15};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, {}, {}, 4).confirmed());
             check(headphones.last_eq_write == Bytes({0x58, 0, 0xa2, 6, 14, 11, 12, 13, 14, 15}));
             check(controller.snapshot().equalizer.preset.value == 0xa2);
             check(controller.snapshot().equalizer.clear_bass.value == 4);
         }},
        {"equalizer-custom-edit-does-not-require-manual-preset",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_preset = 0xa1;
             headphones.eq_presets = {0, 0xa1, 0xa2};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 0, 2).confirmed());
             check(headphones.eq_preset == 0xa1 && headphones.eq_writes == 1);
         }},
        {"equalizer-built-in-edit-requires-advertised-manual-preset",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_presets = {0, 0x10, 0xa1, 0xa2};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 0, 2).code == ResultCode::InvalidValue);
             check(headphones.eq_preset == 0x10 && headphones.eq_writes == 0);
         }},
        {"equalizer-manual-edit-retains-manual-preset",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_preset = 0xa0;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 4, -10, 10).confirmed());
             check(headphones.last_eq_write == Bytes({0x58, 0, 0xa0, 6, 20, 10, 10, 10, 10, 0}));
         }},
        {"equalizer-custom-edit-is-not-confirmed-without-matching-read-back",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_preset = 0xa2;
             headphones.accept_eq = false;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 2, 5).code == ResultCode::DeviceRejected);
             check(headphones.last_eq_write == Bytes({0x58, 0, 0xa2, 6, 10, 10, 10, 15, 10, 10}));
             check(controller.snapshot().equalizer.preset.value == 0xa2);
             check(controller.snapshot().equalizer.bands.value == std::vector<int>({0, 0, 0, 0, 0}));
         }},
        {"equalizer-off-disables-band-writing",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x50};
             headphones.eq_preset = 0;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_equalizer({}, 0, 1).code == ResultCode::Unavailable);
             check(headphones.eq_writes == 0);
             check(controller.set_equalizer(Byte{0x10}).confirmed());
         }},
        {"extended-e7-transition-guard-and-sentinel-candidates",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xe7};
             headphones.switching = 1;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(!controller.snapshot().features.at(Feature::ConnectionPreference).writable);
             check(controller.set(Feature::ConnectionPreference, 1).code == ResultCode::Unavailable);
             check(headphones.extended_writes == 0);
         }},
        {"extended-setting-delivery-needs-target-readback",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xe7};
             headphones.accept_extended = false;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set(Feature::ConnectionPreference, 1).code == ResultCode::DeviceRejected);
             check(controller.snapshot().features.at(Feature::ConnectionPreference).value.value == 0);
         }},
        {"touch-key-availability-prevents-write",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xf3};
             headphones.key_status = 1;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_touch(1, 0xff).code == ResultCode::Unavailable);
             check(headphones.touch_writes == 0);
         }},
        {"touch-unadvertised-actual-assignment-blocks-edit",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xf3};
             headphones.right_assignment = 0x20;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(!controller.snapshot().touch.assignments.known());
             check(controller.set_touch(1, 0x10).code == ResultCode::Unavailable);
         }},
        {"independent-cycle-uses-reported-key-and-actual-assignment",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xf3, 0x69};
             headphones.ordinary_cycle = true;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.snapshot().touch.cycle_writable);
             check(controller.set(Feature::AmbientCycle, 3).confirmed());
             check(controller.set(Feature::AmbientCycle, 0).code == ResultCode::InvalidValue);
         }},
        {"custom-touch-details-preserve-fixed-actions-and-full-list",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xf3};
             headphones.custom_touch = true;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             const auto snapshot = controller.snapshot();
             check(snapshot.touch.details.known());
             auto details = *snapshot.touch.details.value;
             auto missing = details;
             missing[0].actions.pop_back();
             check(controller.set_touch_details(missing).code == ResultCode::InvalidValue);
             auto fixed = details;
             fixed[0].actions[0].function = 0x21;
             check(controller.set_touch_details(fixed).code == ResultCode::InvalidValue);
             auto duplicate = details;
             duplicate[0].actions[1] = duplicate[0].actions[0];
             check(controller.set_touch_details(duplicate).code == ResultCode::InvalidValue);
             details[0].actions[1].function = 2;
             check(controller.set_touch_details(details).confirmed());
             check(headphones.detail_writes == 1);
         }},
        {"linked-touch-disable-captures-and-restores-assistant",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xf3, 0xf4};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.set_touch(1, 0xff).code == ResultCode::ConfirmationRequired);
             check(headphones.touch_writes == 0);
             check(controller.set_touch(1, 0xff, true).confirmed());
             check(controller.snapshot().touch.linked_backup_assistant == 0x30);
             check(controller.restore_touch_linked_backup().confirmed());
             check(headphones.right_assignment == 0x10 && headphones.assistant == 0x30);
             check(!controller.snapshot().touch.linked_backup_assignments);
         }},
        {"guidance-disable-blocks-prompt-volume",
         [] {
             Headphones headphones;
             headphones.functions2 = {0x45, 0x43};
             headphones.guidance = 1;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(!controller.snapshot().touch.volume_writable);
             check(controller.set(Feature::PromptVolume, 1).code == ResultCode::Unavailable);
             check(controller.set(Feature::Notifications, 0).confirmed());
             check(controller.set(Feature::PromptVolume, 2).confirmed());
         }},
        {"guidance-switch-retains-current-language",
         [] {
             for (const auto type : {Byte{1}, Byte{2}}) {
                 Headphones headphones;
                 headphones.guidance_type = type;
                 headphones.functions2 = {static_cast<Byte>(type == 1 ? 0x42 : 0x44)};
                 DeviceController controller(fake_for(headphones), policy());
                 check(controller.connect(device()).ok());
                 check(controller.snapshot().touch.guidance_writable);
                 check(controller.set(Feature::Notifications, 1).confirmed());
                 check(headphones.last_guidance_write == Bytes{0x48, type, 1, 0xf0});
                 check(controller.set(Feature::Notifications, 0).confirmed());
                 check(headphones.guidance_writes == 2);
             }
         }},
        {"guidance-mtk-without-language-switch-uses-on-off-only-payload",
         [] {
             Headphones headphones;
             headphones.guidance_type = 0;
             headphones.functions2 = {0x40};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.snapshot().touch.guidance_writable);
             check(controller.set(Feature::Notifications, 1).confirmed());
             check(headphones.last_guidance_write == Bytes{0x48, 0, 1});
         }},
        {"guidance-readable-state-does-not-imply-switch-support",
         [] {
             for (const auto type : {Byte{0}, Byte{1}, Byte{2}}) {
                 Headphones headphones;
                 headphones.guidance_type = type;
                 headphones.guidance_switch = 0;
                 headphones.functions2 = {static_cast<Byte>(type == 0 ? 0x40 : type == 1 ? 0x41 : 0x44)};
                 DeviceController controller(fake_for(headphones), policy());
                 check(controller.connect(device()).ok());
                 check(controller.snapshot().touch.guidance.known());
                 check(!controller.snapshot().touch.guidance_writable);
                 check(controller.set(Feature::Notifications, 1).code == ResultCode::Unavailable);
                 check(headphones.guidance_writes == 0);
             }
         }},
        {"guidance-language-unknown-blocks-write",
         [] {
             for (const auto type : {Byte{1}, Byte{2}})
                 for (const auto language : {Byte{0}, Byte{0xff}}) {
                     Headphones headphones;
                     headphones.guidance_type = type;
                     headphones.guidance_language = language;
                     headphones.functions2 = {static_cast<Byte>(type == 1 ? 0x41 : 0x44)};
                     DeviceController controller(fake_for(headphones), policy());
                     check(controller.connect(device()).ok());
                     check(!controller.snapshot().touch.guidance_writable);
                     check(controller.set(Feature::Notifications, 1).code == ResultCode::Unavailable);
                     check(headphones.guidance_writes == 0);
                 }
         }},
        {"guidance-unavailable-status-and-reconnect-clear-write-access",
         [] {
             Headphones headphones;
             headphones.guidance_type = 1;
             headphones.functions2 = {0x41};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.snapshot().touch.guidance_writable);
             controller.disconnect();
             headphones.guidance_available = false;
             check(controller.connect(device()).ok());
             check(!controller.snapshot().touch.guidance_writable);
             check(controller.set(Feature::Notifications, 1).code == ResultCode::Unavailable);
             controller.disconnect();
             headphones.guidance_available = true;
             headphones.guidance_switch = 0;
             check(controller.connect(device()).ok());
             check(!controller.snapshot().touch.guidance_writable);
             check(headphones.guidance_writes == 0);
         }},
        {"source-target-fixation-and-codec-dependencies",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xd1, 0x12};
             headphones.functions2 = {0x30, 0x31};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.snapshot().sources.switch_writable);
             check(controller.switch_source("AA:BB:CC:DD:EE:09").code == ResultCode::InvalidValue);
             check(controller.switch_source(headphones.second_source).confirmed());
             check(controller.snapshot().sources.active_source.value == headphones.second_source);
             check(controller.snapshot().codec.value == "AAC");
             check(controller.set(Feature::FixedPlayback, 1).confirmed());
             check(!controller.snapshot().sources.switch_writable);
             check(controller.switch_source(headphones.first_source).code == ResultCode::Unavailable);
             check(controller.set(Feature::FixedPlayback, 0).confirmed());
             check(controller.set(Feature::Multipoint, 0).confirmed());
             check(!controller.snapshot().sources.switch_writable);
         }},
        {"external-source-change-fixation-readback-restores-controls-without-writes",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xd1};
             headphones.functions2 = {0x30, 0x31};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             // Preserve a foreground error while the read-only follow-up runs.
             check(controller.switch_source("invalid-source").code == ResultCode::InvalidValue);
             const auto prior = controller.snapshot().last_result;
             headphones.playback_slot = 2;
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             eventually([&] { return controller.snapshot().sources.active_source.value == headphones.second_source; });
             check(!controller.snapshot().touch.playback_fixed.known());
             check(!controller.snapshot().touch.fixation_writable && !controller.snapshot().sources.switch_writable);
             unsigned reads = 0;
             transport->handler = [&](auto &t, const auto &frame) {
                 check(!controller.snapshot().busy, "Background fixation read must not mark the UI busy");
                 check(controller.poll_playback_fixed().code == ResultCode::Busy,
                       "A second fixation poll must respect the existing operation lock");
                 check(frame.type == table2_type && frame.payload == Bytes{0x36, 1},
                       "Fixation follow-up must only use the existing read command");
                 ++reads;
                 headphones(t, frame);
             };
             check(controller.poll_playback_fixed().ok());
             const auto restored = controller.snapshot();
             check(restored.touch.playback_fixed.known() && !*restored.touch.playback_fixed.value);
             check(restored.touch.fixation_writable && restored.sources.switch_writable);
             check(restored.last_result.code == prior.code && restored.last_result.message == prior.message);
             check(controller.poll_playback_fixed().ok() && reads == 1, "Known fixation must not be polled again");
             check(headphones.source_writes == 0 && headphones.fix_writes == 0);
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.set(Feature::FixedPlayback, 1).confirmed());
             check(controller.set(Feature::FixedPlayback, 0).confirmed());
             check(controller.switch_source(headphones.first_source).confirmed());
         }},
        {"fixation-readback-discards-source-away-and-back-generation",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xd1};
             headphones.functions2 = {0x30, 0x31};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             headphones.playback_slot = 2;
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             eventually([&] { return controller.snapshot().sources.active_source.value == headphones.second_source; });
             transport->handler = [&](auto &t, const auto &frame) {
                 check(frame.payload == Bytes{0x36, 1});
                 t.push({table2_type, 0, source_notification(headphones, 1)});
                 t.push({table2_type, 1, source_notification(headphones, 2)});
                 t.reply(frame, {0x37, 1, 0}); // An obsolete source generation says fixed.
             };
             check(controller.poll_playback_fixed().code == ResultCode::Canceled);
             check(controller.snapshot().sources.active_source.value == headphones.second_source);
             check(!controller.snapshot().touch.playback_fixed.known());
             check(!controller.snapshot().touch.fixation_writable && !controller.snapshot().sources.switch_writable);
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.poll_playback_fixed().ok());
             check(controller.snapshot().touch.playback_fixed.known() && !*controller.snapshot().touch.playback_fixed.value);
             check(controller.snapshot().sources.switch_writable);
             check(headphones.source_writes == 0 && headphones.fix_writes == 0);
         }},
        {"fixation-readback-requires-current-link-capability-multipoint-and-source",
         [] {
             for (int condition = 0; condition < 4; ++condition) {
                 Headphones headphones;
                 headphones.functions1 = {0xd1};
                 headphones.functions2 = condition == 0 ? Bytes{0x30} : Bytes{0x30, 0x31};
                 if (condition == 1) headphones.multipoint = false;
                 auto fake = fake_for(headphones);
                 auto *transport = fake.get();
                 DeviceController controller(std::move(fake), policy());
                 check(controller.connect(device()).ok());
                 if (condition == 2) controller.disconnect();
                 if (condition == 3) {
                     transport->push({table2_type, 0, source_notification(headphones, 0)});
                     eventually([&] { return !controller.snapshot().sources.active_source.known(); });
                 }
                 const auto requests = transport->requests.load();
                 check(controller.poll_playback_fixed().code == ResultCode::Unavailable);
                 check(transport->requests == requests && headphones.source_writes == 0 && headphones.fix_writes == 0);
             }
         }},
        {"fixation-readback-rejects-malformed-data-and-keeps-unknown-controls-disabled",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xd1};
             headphones.functions2 = {0x30, 0x31};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             eventually([&] { return controller.snapshot().sources.active_source.value == headphones.second_source; });
             transport->handler = [&](auto &t, const auto &frame) { t.reply(frame, {0x37, 1, 0xff}); };
             transport->fail_opens = 9;
             check(!controller.poll_playback_fixed().ok(), "Malformed fixation must not confirm the readback");
             check(!controller.snapshot().touch.playback_fixed.known(), "Malformed fixation must remain unknown");
             check(!controller.snapshot().touch.fixation_writable && !controller.snapshot().sources.switch_writable,
                   "Malformed fixation must not enable source writes");
             check(headphones.source_writes == 0 && headphones.fix_writes == 0);
             // A timed-out exchange poisons its channel by design. Retain the
             // existing recovery contract instead of bypassing that check.
             controller.disconnect();
             transport->fail_opens = 0;
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.connect(device()).ok());
             check(controller.poll_playback_fixed().ok());
             check(controller.snapshot().touch.fixation_writable);
         }},
        {"source-switch-rejection-not-hidden-by-ack",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xd1};
             headphones.functions2 = {0x30, 0x31};
             headphones.reject_source = true;
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             check(controller.switch_source(headphones.second_source).code == ResultCode::DeviceRejected);
             check(controller.snapshot().sources.active_source.value == headphones.first_source);
         }},
        {"source-change-blocks-late-codec-until-current-query-succeeds",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             check(controller.snapshot().codec.value == "AAC");
             const auto revision = controller.snapshot().revision;
             headphones.playback_slot = 2;
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             transport->push({table1_type, 1, {0x15, 2, 2}});
             eventually([&] { return controller.snapshot().revision >= revision + 2; });
             check(controller.snapshot().sources.active_source.value == headphones.second_source);
             check(!controller.snapshot().codec.known(), "Late old-source codec restored Known");
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12)
                     t.reply(frame, {0x13, 2, 0x10});
                 else
                     headphones(t, frame);
             };
             check(controller.refresh().ok());
             check(controller.snapshot().codec.value == "LDAC");
             transport->push({table1_type, 0, {0x15, 2, 0x30}});
             eventually([&] { return controller.snapshot().codec.value == "LC3"; });
         }},
        {"background-codec-poll-recovers-external-source-without-foreground-result-change",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             const auto before = controller.snapshot().last_result;
             check(controller.poll_codec().ok());
             headphones.playback_slot = 2;
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             eventually([&] { return controller.snapshot().sources.active_source.value == headphones.second_source; });
             check(!controller.snapshot().codec.known());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     check(controller.poll_codec().code == ResultCode::Busy, "Poll must not overlap an existing query");
                     t.reply(frame, {0x13, 2, 0x10});
                 }
                 else headphones(t, frame);
             };
             const auto requests = transport->requests.load();
             check(controller.poll_codec().ok());
             check(controller.snapshot().codec.value == "LDAC");
             check(transport->requests == requests + 1, "Background poll must only read the codec");
             check(controller.snapshot().last_result.code == before.code && controller.snapshot().last_result.message == before.message);
             check(controller.poll_codec().ok() && transport->requests == requests + 1,
                   "Known codec must not trigger repeated queries");
             controller.disconnect();
             check(!controller.poll_codec().ok() && transport->requests == requests + 1);
         }},
        {"background-codec-poll-rejects-changed-generation-and-retries-current-source",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             headphones.playback_slot = 2;
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             eventually([&] { return controller.snapshot().sources.active_source.value == headphones.second_source; });
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     t.push({table2_type, 1, source_notification(headphones, 1)});
                     t.reply(frame, {0x13, 2, 0x10});
                 } else headphones(t, frame);
             };
             check(controller.poll_codec().code == ResultCode::Superseded);
             check(!controller.snapshot().codec.known(), "Old-source reply must not release the fence");
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.poll_codec().ok() && controller.snapshot().codec.value == "AAC");
         }},
        {"codec-query-discards-reply-after-source-changes-away-and-back",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     t.push({table2_type, 0, source_notification(headphones, 2)});
                     t.push({table2_type, 1, source_notification(headphones, 1)});
                     t.reply(frame, {0x13, 2, 2});
                 } else
                     headphones(t, frame);
             };
             check(controller.refresh().ok());
             check(controller.snapshot().sources.active_source.value == headphones.first_source);
             check(!controller.snapshot().codec.known(), "An earlier source generation accepted a reply");
             const auto revision = controller.snapshot().revision;
             transport->push({table1_type, 0, {0x15, 2, 0x10}});
             eventually([&] { return controller.snapshot().revision > revision; });
             check(!controller.snapshot().codec.known());
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.refresh().ok());
             check(controller.snapshot().codec.value == "AAC");
         }},
        {"codec-response-before-delayed-ack-retains-newer-notification",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             auto test_policy = policy();
             test_policy.request_timeout = Milliseconds{500};
             DeviceController controller(std::move(fake), test_policy);
             std::future<void> delivery;
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     const auto revision = controller.snapshot().revision;
                     t.push({table1_type, 0, {0x13, 2, 2}});
                     t.push({table1_type, 1, {0x15, 2, 0x10}});
                     // The synchronous handler runs under the production write
                     // lock. Return so the reader can ACK and apply these frames.
                     delivery = std::async(std::launch::async, [&t, request = frame, revision, &controller] {
                         eventually([&] { return controller.snapshot().revision > revision; });
                         check(!controller.snapshot().codec.known(),
                               "A notification released the codec fence before query ACK");
                         t.acknowledge(request);
                     });
                 } else
                     headphones(t, frame);
             };
             const auto result = controller.refresh();
             check(delivery.valid(), "Delayed ACK producer was not started");
             delivery.get();
             check(result.ok(), "Codec refresh failed in the delayed ACK scenario");
             check(controller.snapshot().codec.value == "LDAC",
                   "Newer codec notification was lost while waiting for the query ACK");
             check(controller.snapshot().codec.notification_observed);
         }},
        {"codec-notification-before-response-cannot-override-current-query",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             auto test_policy = policy();
             test_policy.request_timeout = Milliseconds{500};
             DeviceController controller(std::move(fake), test_policy);
             std::future<void> delivery;
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     const auto revision = controller.snapshot().revision;
                     t.push({table1_type, 0, {0x15, 2, 0x10}});
                     delivery = std::async(std::launch::async, [&t, request = frame, revision, &controller] {
                         eventually([&] { return controller.snapshot().revision > revision; });
                         check(!controller.snapshot().codec.known(),
                               "A pre-response notification released the codec fence");
                         t.reply(request, {0x13, 2, 2});
                     });
                 } else
                     headphones(t, frame);
             };
             const auto result = controller.refresh();
             check(delivery.valid(), "Delayed codec response producer was not started");
             delivery.get();
             check(result.ok(), "Codec refresh failed after an earlier notification");
             check(controller.snapshot().codec.value == "AAC",
                   "An earlier buffered notification overrode the successful query");
         }},
        {"source-generation-change-discards-post-response-buffer-before-ack",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             auto test_policy = policy();
             test_policy.request_timeout = Milliseconds{500};
             DeviceController controller(std::move(fake), test_policy);
             std::future<void> delivery;
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     const auto revision = controller.snapshot().revision;
                     t.push({table1_type, 0, {0x13, 2, 2}});
                     t.push({table1_type, 1, {0x15, 2, 0x10}});
                     t.push({table2_type, 0, source_notification(headphones, 2)});
                     t.push({table2_type, 1, source_notification(headphones, 1)});
                     delivery = std::async(std::launch::async, [&t, request = frame, revision, &controller] {
                         eventually([&] { return controller.snapshot().revision >= revision + 3; });
                         check(!controller.snapshot().codec.known(),
                               "A changed source generation accepted a buffered codec before ACK");
                         t.acknowledge(request);
                     });
                 } else
                     headphones(t, frame);
             };
             const auto result = controller.refresh();
             check(delivery.valid(), "Source-change delayed ACK producer was not started");
             delivery.get();
             check(result.ok(), "Codec refresh failed in the source generation scenario");
             check(controller.snapshot().sources.active_source.value == headphones.first_source);
             check(!controller.snapshot().codec.known(),
                   "A buffered codec from an obsolete source generation was restored");
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.refresh().ok());
             check(controller.snapshot().codec.value == "AAC");
         }},
        {"codec-query-failure-keeps-new-source-codec-unknown",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             headphones.playback_slot = 2;
             const auto revision = controller.snapshot().revision;
             transport->push({table2_type, 0, source_notification(headphones, 2)});
             transport->push({table1_type, 1, {0x15, 2, 2}});
             eventually([&] { return controller.snapshot().revision >= revision + 2; });
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x12) {
                     t.acknowledge(frame);
                     t.push({table1_type, 0, {0x15, 2, 2}});
                 } else
                     headphones(t, frame);
             };
             transport->fail_opens = 9;
             check(controller.refresh().code == ResultCode::Timeout);
             check(!controller.snapshot().codec.known());
             check(!controller.snapshot().codec.value.has_value());
             controller.disconnect();
             transport->fail_opens = 0;
             transport->handler = [&](auto &t, const auto &frame) { headphones(t, frame); };
             check(controller.connect(device()).ok());
             check(controller.snapshot().codec.value == "AAC");
         }},
        {"connection-preference-blocks-codec-notification-before-readback",
         [] {
             Headphones headphones;
             headphones.functions1 = {0xe7, 0x12};
             headphones.functions2 = {0x30};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0xe8) {
                     headphones(t, frame);
                     t.push({table1_type, 0, {0x15, 2, 2}});
                 } else if (frame.payload[0] == 0x12) {
                     check(!controller.snapshot().codec.known(),
                           "Quality-change notification bypassed the query fence");
                     t.reply(frame, {0x13, 2, 0x10});
                 } else
                     headphones(t, frame);
             };
             check(controller.set(Feature::ConnectionPreference, 1).confirmed());
             check(controller.snapshot().codec.value == "LDAC");
         }},
        {"newer-notification-retained-over-query-readback",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x6b};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 headphones(t, frame);
                 if (frame.payload[0] == 0x66 && frame.payload[1] == 0x17)
                     t.push({table1_type, 1, {0x69, 0x17, 1, 1, 1, 0, 18}});
             };
             check(controller.refresh().ok());
             eventually([&] { return controller.snapshot().sound.ambient_level.value == 18; });
             check(controller.snapshot().sound.ambient_level.notification_observed);
         }},
        {"suspend-aborts-write-and-resume-only-reloads-readback",
         [] {
             Headphones headphones;
             headphones.functions1 = {0x6b};
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->handler = [&](auto &t, const auto &frame) {
                 if (frame.payload[0] == 0x68) {
                     ++headphones.sound_writes;
                     headphones.sound.level = frame.payload[6];
                     return;
                 }
                 headphones(t, frame);
             };
             auto write = std::async(std::launch::async, [&] { return controller.set_sound({}, 11); });
             eventually([&] { return headphones.sound_writes == 1; });
             const auto before = std::chrono::steady_clock::now();
             controller.suspend();
             check(std::chrono::steady_clock::now() - before < Milliseconds{50},
                   "Suspend blocked instead of immediately closing");
             check(write.get().code == ResultCode::NotConnected);
             check(!controller.snapshot().ready);
             check(controller.resume().ok());
             eventually([&] { return controller.snapshot().ready; });
             check(transport->opens == 2);
             check(headphones.sound_writes == 1);
             check(controller.snapshot().sound.ambient_level.value == 11);
         }},
        {"adapter-recovery-bounded-and-manual-disconnect-cancels-resume",
         [] {
             Headphones headphones;
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             controller.notify_bluetooth_available(false);
             check(!controller.snapshot().connected);
             controller.notify_bluetooth_available(true);
             eventually([&] { return controller.snapshot().ready; });
             check(transport->opens == 2);
             controller.suspend();
             controller.disconnect();
             controller.resume();
             std::this_thread::sleep_for(Milliseconds{150});
             check(transport->opens == 2);
         }},
        {"unexpected-link-loss-recovery-budget-stops-after-failure",
         [] {
             Headphones headphones;
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->fail_opens = 9;
             transport->close();
             eventually([&] { return transport->opens == 3 && !controller.snapshot().busy; });
             std::this_thread::sleep_for(Milliseconds{180});
             check(transport->opens == 3);
             check(!controller.snapshot().ready);
         }},
        {"manual-device-connection-is-not-overwritten-by-background-recovery",
         [] {
             Headphones headphones;
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             std::promise<void> opened, release;
             const auto released = release.get_future().share();
             transport->before_open = [&](const auto &) {
                 if (transport->opens == 2) {
                     opened.set_value();
                     check(released.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                           "Manual connection pause was not released");
                 }
             };
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             auto selected = device();
             ++selected.address;
             selected.identity = "BTH:112233445567";
             auto connect = std::async(std::launch::async, [&] { return controller.connect(selected); });
             check(opened.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
             // Several recovery wakeups occur while the manual operation owns the lock.
             std::this_thread::sleep_for(Milliseconds{220});
             release.set_value();
             check(connect.get().ok());
             std::this_thread::sleep_for(Milliseconds{220});
             check(controller.snapshot().ready);
             check(controller.snapshot().device->address == selected.address);
             check(transport->last_open_address == selected.address);
             check(transport->opens == 2, "Recovery reconnected the previous selection");
         }},
        {"suspend-during-recovery-does-not-publish-obsolete-completion",
         [] {
             Headphones headphones;
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             std::promise<void> opened, release;
             const auto released = release.get_future().share();
             transport->before_open = [&](const auto &) {
                 if (transport->opens == 2) {
                     opened.set_value();
                     check(released.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                           "Recovery pause was not released");
                 }
             };
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->close();
             check(opened.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
             controller.suspend();
             const auto suspended_status = controller.snapshot().status;
             release.set_value();
             eventually([&] { return !controller.snapshot().busy; });
             check(controller.snapshot().status == suspended_status);
             check(!controller.snapshot().connected && !controller.snapshot().ready);
             std::this_thread::sleep_for(Milliseconds{150});
             check(transport->opens == 2);
         }},
        {"manual-disconnect-cancels-running-recovery-and-retains-disconnected-status",
         [] {
             Headphones headphones;
             auto fake = fake_for(headphones);
             auto *transport = fake.get();
             std::promise<void> opened, release;
             const auto released = release.get_future().share();
             transport->before_open = [&](const auto &) {
                 if (transport->opens == 2) {
                     opened.set_value();
                     check(released.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                           "Recovery pause was not released");
                 }
             };
             DeviceController controller(std::move(fake), policy());
             check(controller.connect(device()).ok());
             transport->close();
             check(opened.get_future().wait_for(std::chrono::seconds(1)) == std::future_status::ready);
             const auto closes = transport->closes.load();
             auto disconnect = std::async(std::launch::async, [&] { controller.disconnect(); });
             eventually([&] { return transport->closes > closes; });
             release.set_value();
             disconnect.get();
             eventually([&] { return !controller.snapshot().busy; });
             std::this_thread::sleep_for(Milliseconds{150});
             check(controller.snapshot().status == "Disconnected");
             check(!controller.snapshot().connected && !controller.snapshot().ready);
             check(transport->opens == 2);
         }},
        {"disconnect-preserves-values-and-disables-write-flags", [] {
             Headphones headphones;
             headphones.functions1 = {0x6b};
             DeviceController controller(fake_for(headphones), policy());
             check(controller.connect(device()).ok());
             controller.disconnect();
             const auto snapshot = controller.snapshot();
             check(!snapshot.ready && !snapshot.sound.writable);
             check(snapshot.sound.ambient_level.value == 7);
             check(controller.set_sound({}, 10).code == ResultCode::Unavailable);
         }}};
    unsigned passed = 0, selected = 0;
    for (const auto &[name, test] : tests) {
        if (!filter.empty() && std::string(name).find(filter) == std::string::npos) continue;
        ++selected;
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception &error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << '/' << selected
              << " offline session scenarios passed. HardwareVerified=false\n";
    return selected && passed == selected ? 0 : 1;
}
