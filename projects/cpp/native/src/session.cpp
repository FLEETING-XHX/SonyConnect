#include "sonyconnect/session.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#endif

namespace sonyconnect {
using namespace protocol;
namespace {
Result failure(ResultCode code, std::string text) { return {code, std::move(text)}; }
void validate_policy(const SessionPolicy &p) {
    if (p.connect_timeout <= Milliseconds::zero() || p.request_timeout <= Milliseconds::zero() ||
        p.write_timeout <= Milliseconds::zero() || p.connect_timeout > std::chrono::minutes(1) ||
        p.request_timeout > std::chrono::minutes(1) || p.maximum_connect_attempts < 1 ||
        p.maximum_connect_attempts > 5 || p.maximum_query_attempts < 1 || p.maximum_query_attempts > 3 ||
        p.retry_delay < Milliseconds::zero() || p.retry_delay > std::chrono::seconds(5))
        throw std::invalid_argument("Invalid bounded session policy");
}
template <class T> void observe(Value<T> &value, T data, std::uint64_t order, bool notification) {
    if (order <= value.receive_order)
        return;
    value.status = ValueStatus::Known;
    value.value = std::move(data);
    value.receive_order = order;
    value.notification_observed |= notification;
    value.reason.clear();
}
template <class T> void unknown(Value<T> &value, std::string reason, std::uint64_t order = 0) {
    if (order && order <= value.receive_order)
        return;
    value.status = ValueStatus::Unknown;
    value.value.reset();
    value.reason = std::move(reason);
    if (order)
        value.receive_order = order;
}
template <class T> void stale(Value<T> &value) {
    if (value.status == ValueStatus::Known) {
        value.status = ValueStatus::Stale;
        value.reason = "Control channel is disconnected; this is the last observed value.";
    }
}
#ifdef _WIN32
// Same V2 MDR service used by the existing C# connector and pinned MIT source.
constexpr GUID sony_service{0x956c7b26, 0xd49a, 0x4ba8, {0xb0, 0x3f, 0xb1, 0x7d, 0x39, 0x3c, 0xb6, 0xe2}};
struct WinsockLifetime {
    WinsockLifetime() {
        WSADATA data{};
        const auto error = WSAStartup(MAKEWORD(2, 2), &data);
        if (error)
            throw std::runtime_error("WSAStartup: " + std::to_string(error));
    }
    ~WinsockLifetime() { WSACleanup(); }
};
void ensure_winsock() { static WinsockLifetime lifetime; }
std::string utf8(const wchar_t *input) {
    const int count = WideCharToMultiByte(CP_UTF8, 0, input, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1)
        return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input, -1, result.data(), count, nullptr, nullptr);
    result.pop_back();
    return result;
}
std::string identity(std::uint64_t address) {
    std::ostringstream text;
    text << "BTH:" << std::uppercase << std::hex << std::setw(12) << std::setfill('0') << address;
    return text.str();
}
bool cached_sony_service(std::uint64_t address) {
    ensure_winsock();
    SOCKADDR_BTH endpoint{};
    endpoint.addressFamily = AF_BTH;
    endpoint.btAddr = address;
    std::array<wchar_t, 128> context{};
    DWORD length = static_cast<DWORD>(context.size());
    if (WSAAddressToStringW(reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint), nullptr,
                            context.data(), &length))
        return false;
    GUID service = sony_service;
    WSAQUERYSETW restriction{};
    restriction.dwSize = sizeof(restriction);
    restriction.dwNameSpace = NS_BTH;
    restriction.lpServiceClassId = &service;
    restriction.lpszContext = context.data();
    HANDLE lookup = nullptr;
    // Cached discovery only. Connection by service UUID performs the actual SDP
    // query inside the bounded socket connection, never inside device listing.
    if (WSALookupServiceBeginW(&restriction, LUP_RETURN_ADDR, &lookup))
        return false;
    alignas(WSAQUERYSETW) std::array<Byte, 65536> storage{};
    DWORD size = static_cast<DWORD>(storage.size());
    auto *found = reinterpret_cast<WSAQUERYSETW *>(storage.data());
    found->dwSize = sizeof(*found);
    const auto result = WSALookupServiceNextW(lookup, LUP_RETURN_ADDR, &size, found);
    WSALookupServiceEnd(lookup);
    return result == 0 && found->dwNumberOfCsAddrs > 0;
}
bool ready_socket(SOCKET socket, bool writing, Milliseconds timeout) {
    fd_set selected{}, failed{};
    FD_ZERO(&selected);
    FD_ZERO(&failed);
    FD_SET(socket, &selected);
    FD_SET(socket, &failed);
    timeval duration{static_cast<long>(timeout.count() / 1000),
                     static_cast<long>((timeout.count() % 1000) * 1000)};
    const auto result =
        select(0, writing ? nullptr : &selected, writing ? &selected : nullptr, &failed, &duration);
    if (result == SOCKET_ERROR)
        throw std::runtime_error("RFCOMM select: " + std::to_string(WSAGetLastError()));
    if (FD_ISSET(socket, &failed)) {
        int error = 0, length = sizeof(error);
        getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length);
        throw std::runtime_error("RFCOMM socket: " + std::to_string(error));
    }
    return result > 0 && FD_ISSET(socket, &selected);
}
class WindowsRfcommTransport final : public ByteTransport {
  public:
    ~WindowsRfcommTransport() override { close(); }
    Result open(const DeviceDescriptor &device, Milliseconds timeout) override {
        close();
        if (!device.paired || !device.address || (device.address >> 48))
            return failure(ResultCode::InvalidValue, "Select a paired Bluetooth device.");
        try {
            ensure_winsock();
            const auto socket = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
            if (socket == INVALID_SOCKET)
                return failure(ResultCode::Transport,
                               "RFCOMM socket creation: " + std::to_string(WSAGetLastError()));
            socket_.store(socket);
            ULONG enabled = TRUE;
            if (setsockopt(socket, SOL_RFCOMM, SO_BTH_AUTHENTICATE, reinterpret_cast<const char *>(&enabled),
                           sizeof(enabled)) ||
                setsockopt(socket, SOL_RFCOMM, SO_BTH_ENCRYPT, reinterpret_cast<const char *>(&enabled),
                           sizeof(enabled)))
                throw std::runtime_error("RFCOMM authentication/encryption: " +
                                         std::to_string(WSAGetLastError()));
            u_long nonblocking = 1;
            if (ioctlsocket(socket, FIONBIO, &nonblocking))
                throw std::runtime_error("RFCOMM nonblocking mode: " + std::to_string(WSAGetLastError()));
            SOCKADDR_BTH endpoint{};
            endpoint.addressFamily = AF_BTH;
            endpoint.btAddr = device.address;
            endpoint.serviceClassId = sony_service;
            const auto result =
                ::connect(socket, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint));
            if (result == SOCKET_ERROR) {
                const auto error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEALREADY)
                    throw std::runtime_error("Sony MDR service connection: " + std::to_string(error));
                if (!ready_socket(socket, true, timeout)) {
                    close();
                    return failure(ResultCode::Timeout, "Sony MDR service connection timed out.");
                }
                int connect_error = 0, length = sizeof(connect_error);
                if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&connect_error),
                               &length) ||
                    connect_error)
                    throw std::runtime_error(
                        "Sony MDR service connection: " +
                        std::to_string(connect_error ? connect_error : WSAGetLastError()));
            }
            if (socket_.load() != socket)
                return failure(ResultCode::Canceled, "Connection canceled.");
            return {};
        } catch (const std::exception &error) {
            close();
            return failure(ResultCode::Transport, error.what());
        }
    }
    ReadResult read(std::span<Byte> output, Milliseconds timeout) override {
        const auto socket = socket_.load();
        if (socket == INVALID_SOCKET)
            return {ReadStatus::Closed};
        if (!ready_socket(socket, false, timeout))
            return {ReadStatus::Timeout};
        const auto count =
            recv(socket, reinterpret_cast<char *>(output.data()), static_cast<int>(output.size()), 0);
        if (!count)
            return {ReadStatus::Closed};
        if (count == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK)
                return {ReadStatus::Timeout};
            throw std::runtime_error("RFCOMM receive: " + std::to_string(error));
        }
        return {ReadStatus::Data, static_cast<std::size_t>(count)};
    }
    void write(std::span<const Byte> bytes, Milliseconds timeout) override {
        if (bytes.size() > 262144)
            throw std::invalid_argument("RFCOMM write exceeds local bound");
        const auto socket = socket_.load();
        if (socket == INVALID_SOCKET)
            throw std::runtime_error("RFCOMM disconnected");
        const auto end = std::chrono::steady_clock::now() + timeout;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            auto remaining = std::chrono::duration_cast<Milliseconds>(end - std::chrono::steady_clock::now());
            if (remaining <= Milliseconds::zero() || !ready_socket(socket, true, remaining)) {
                close();
                throw std::runtime_error("RFCOMM write timed out; partial stream closed");
            }
            const auto sent = send(socket, reinterpret_cast<const char *>(bytes.data() + offset),
                                   static_cast<int>(bytes.size() - offset), 0);
            if (sent == SOCKET_ERROR) {
                const auto error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK)
                    continue;
                close();
                throw std::runtime_error("RFCOMM send: " + std::to_string(error));
            }
            if (!sent) {
                close();
                throw std::runtime_error("RFCOMM send returned zero");
            }
            offset += static_cast<std::size_t>(sent);
        }
    }
    void close() noexcept override {
        const auto socket = socket_.exchange(INVALID_SOCKET);
        if (socket != INVALID_SOCKET) {
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
    }

  private:
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
};
#else
class UnavailableTransport final : public ByteTransport {
    Result open(const DeviceDescriptor &, Milliseconds) override {
        return failure(ResultCode::Unavailable, "Native RFCOMM requires Windows.");
    }
    ReadResult read(std::span<Byte>, Milliseconds) override { return {ReadStatus::Closed}; }
    void write(std::span<const Byte>, Milliseconds) override {
        throw std::runtime_error("Native RFCOMM requires Windows");
    }
    void close() noexcept override {}
};
#endif
} // namespace

bool is_connected_sony_device(const DeviceDescriptor &device) {
    return device.paired && device.connected_hint && device.address && !(device.address >> 48) &&
           (device.name == "WF-1000XM5" || device.name == "WH-1000XM5" || device.sony_service_hint);
}
DiscoveryResult discover_paired_devices() {
    DiscoveryResult result;
#ifdef _WIN32
    BLUETOOTH_FIND_RADIO_PARAMS parameters{};
    parameters.dwSize = sizeof(parameters);
    HANDLE radio = nullptr;
    const auto enumeration = BluetoothFindFirstRadio(&parameters, &radio);
    if (!enumeration) {
        result.result = failure(ResultCode::Unavailable,
                                "No accessible Bluetooth radio: " + std::to_string(GetLastError()));
        return result;
    }
    std::set<std::uint64_t> addresses;
    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
        search.dwSize = sizeof(search);
        search.fReturnConnected = TRUE;
        search.hRadio = radio;
        search.fIssueInquiry = FALSE;
        BLUETOOTH_DEVICE_INFO info{};
        info.dwSize = sizeof(info);
        const auto devices = BluetoothFindFirstDevice(&search, &info);
        if (devices) {
            do {
                if (!info.fAuthenticated || !info.fConnected || addresses.contains(info.Address.ullLong))
                    continue;
                const auto name = utf8(info.szName);
                DWORD count = 0;
                bool has_service = false;
                if (info.fConnected) {
                    const auto first = BluetoothEnumerateInstalledServices(radio, &info, &count, nullptr);
                    if ((first == ERROR_SUCCESS || first == ERROR_MORE_DATA) && count > 0 && count <= 256) {
                        std::vector<GUID> services(count);
                        if (BluetoothEnumerateInstalledServices(radio, &info, &count, services.data()) ==
                            ERROR_SUCCESS)
                            has_service =
                                std::any_of(services.begin(), services.begin() + count, [](const GUID &item) {
                                    return InlineIsEqualGUID(item, sony_service);
                                });
                    }
                    if (!has_service)
                        try {
                            has_service = cached_sony_service(info.Address.ullLong);
                        } catch (const std::exception &) {
                        }
                }
                DeviceDescriptor candidate{
                    info.Address.ullLong, name, identity(info.Address.ullLong), true, true, has_service};
                if (is_connected_sony_device(candidate)) {
                    addresses.insert(info.Address.ullLong);
                    result.devices.push_back(std::move(candidate));
                }
            } while (result.devices.size() < 256 && BluetoothFindNextDevice(devices, &info));
            BluetoothFindDeviceClose(devices);
        } else if (GetLastError() != ERROR_NO_MORE_ITEMS)
            result.result = failure(ResultCode::Transport,
                                    "Connected Bluetooth discovery: " + std::to_string(GetLastError()));
        CloseHandle(radio);
        radio = nullptr;
    } while (result.devices.size() < 256 && BluetoothFindNextRadio(enumeration, &radio));
    BluetoothFindRadioClose(enumeration);
    std::sort(result.devices.begin(), result.devices.end(), [](const auto &a, const auto &b) {
        return a.connected_hint != b.connected_hint ? a.connected_hint > b.connected_hint : a.name < b.name;
    });
#else
    result.result = failure(ResultCode::Unavailable, "Connected Bluetooth discovery requires Windows.");
#endif
    return result;
}
std::unique_ptr<ByteTransport> make_windows_rfcomm_transport() {
#ifdef _WIN32
    return std::make_unique<WindowsRfcommTransport>();
#else
    return std::make_unique<UnavailableTransport>();
#endif
}

struct ProtocolSession::Impl {
    std::unique_ptr<ByteTransport> transport;
    SessionPolicy policy;
    mutable std::mutex gate;
    std::mutex operations, writes, lifecycle;
    std::condition_variable changed;
    std::thread receiver;
    StreamDecoder decoder;
    bool online{}, poisoned{};
    std::uint64_t identity{}, order{}, cancellation_revision{};
    Byte sequence{};
    Family family{Family::Unknown};
    std::optional<Frame> last_incoming;
    struct Pending {
        Query query;
        Byte sequence{};
        bool ack{};
        std::optional<Frame> response;
        std::uint64_t order{};
    };
    std::optional<Pending> pending;
    std::function<void(const Observation &)> observer;
    std::function<void(const ProtocolTrace &)> trace;
    std::string error;
    Impl(std::unique_ptr<ByteTransport> source, SessionPolicy p) : transport(std::move(source)), policy(p) {
        validate_policy(p);
        if (!transport)
            throw std::invalid_argument("Missing transport");
    }
    void emit(std::string event, std::optional<Frame> frame = {}, std::uint64_t receive_order = 0) {
        std::function<void(const ProtocolTrace &)> handler;
        std::uint64_t link;
        std::string reason;
        {
            std::lock_guard lock(gate);
            handler = trace;
            link = identity;
            if (event == "link-failed") reason = error;
        }
        if (handler)
            try {
                handler({std::move(event), link, receive_order,
                         frame ? std::optional<Byte>{frame->type} : std::nullopt,
                         frame ? std::optional<Byte>{frame->sequence} : std::nullopt,
                         frame && !frame->payload.empty() ? std::optional<Byte>{frame->payload.front()}
                                                          : std::nullopt, std::move(reason)});
            } catch (...) {
            }
    }
    void fail(std::string message) {
        {
            std::lock_guard lock(gate);
            online = false;
            poisoned = true;
            error = std::move(message);
        }
        transport->close();
        changed.notify_all();
        emit("link-failed");
    }
    void write(const Frame &frame) {
        const auto bytes = pack(frame);
        std::lock_guard lock(writes);
        transport->write(bytes, policy.write_timeout);
    }
    void receive() {
        std::array<Byte, 4096> bytes{};
        try {
            for (;;) {
                {
                    std::lock_guard lock(gate);
                    if (!online)
                        return;
                }
                auto read = transport->read(bytes, Milliseconds{100});
                if (read.status == ReadStatus::Timeout)
                    continue;
                if (read.status == ReadStatus::Closed) {
                    fail("Headphones closed the RFCOMM control channel.");
                    return;
                }
                if (!read.count || read.count > bytes.size()) {
                    fail("Transport returned an invalid receive length.");
                    return;
                }
                for (const auto &decoded : decoder.feed(std::span<const Byte>{bytes}.first(read.count))) {
                    if (!decoded.frame) {
                        emit("bad-frame");
                        continue;
                    }
                    const auto &frame = *decoded.frame;
                    if (frame.requires_acknowledgement() && frame.sequence <= 1) {
                        write(acknowledgement_for(frame));
                        emit("ack-sent", frame);
                    }
                    Family parsing_family;
                    {
                        std::lock_guard lock(gate);
                        parsing_family = family;
                    }
                    const auto parsed = parse(frame, parsing_family);
                    emit(parsed.valid() ? "receive" : "malformed-message", frame);
                    if (!parsed.valid())
                        continue;
                    std::function<void(const Observation &)> handler;
                    std::optional<Observation> observation;
                    {
                        std::lock_guard lock(gate);
                        if (!online || poisoned)
                            continue;
                        if (parsed.message.kind == Kind::Acknowledgement) {
                            if (pending && frame.sequence == 1 - pending->sequence)
                                pending->ack = true;
                            changed.notify_all();
                            continue;
                        }
                        if (frame.requires_acknowledgement()) {
                            if (last_incoming && *last_incoming == frame)
                                continue;
                            last_incoming = frame;
                        }
                        const auto receive_order = ++order;
                        if (pending && pending->query.await_response &&
                            parsed.message.kind == Kind::Response && frame.type == pending->query.type &&
                            parsed.message.command == pending->query.response_command &&
                            frame.payload.size() >= 2 && frame.payload[1] == pending->query.inquired_type &&
                            !pending->response) {
                            if (const auto info =
                                    std::get_if<ProtocolInformation>(&parsed.message.information))
                                family = info->family;
                            pending->response = frame;
                            pending->order = receive_order;
                            changed.notify_all();
                        } else if (parsed.message.kind == Kind::Notification) {
                            handler = observer;
                            observation = Observation{parsed.message, identity, receive_order};
                        }
                    }
                    if (handler && observation)
                        try {
                            handler(*observation);
                        } catch (...) {
                            emit("observer-error");
                        }
                }
            }
        } catch (const std::exception &exception) {
            fail(exception.what());
        }
    }
    void stop() noexcept {
        {
            std::lock_guard lock(gate);
            online = false;
            poisoned = true;
        }
        transport->close();
        changed.notify_all();
        if (receiver.joinable() && receiver.get_id() != std::this_thread::get_id())
            receiver.join();
    }
};
ProtocolSession::ProtocolSession(std::unique_ptr<ByteTransport> transport, SessionPolicy policy)
    : impl_(std::make_unique<Impl>(std::move(transport), policy)) {}
ProtocolSession::~ProtocolSession() { disconnect(); }
Result ProtocolSession::connect(const DeviceDescriptor &device) {
    std::uint64_t cancellation_revision;
    {
        std::lock_guard lock(impl_->gate);
        cancellation_revision = impl_->cancellation_revision;
    }
    std::unique_lock lifecycle(impl_->lifecycle);
    impl_->stop();
    std::unique_lock operations(impl_->operations);
    Result result;
    for (unsigned attempt = 0; attempt < impl_->policy.maximum_connect_attempts; ++attempt) {
        {
            std::lock_guard lock(impl_->gate);
            if (cancellation_revision != impl_->cancellation_revision)
                return failure(ResultCode::Canceled, "Connection canceled.");
        }
        result = impl_->transport->open(device, impl_->policy.connect_timeout);
        if (result.ok() || result.code == ResultCode::InvalidValue ||
            result.code == ResultCode::Unavailable || result.code == ResultCode::Canceled)
            break;
        if (attempt + 1 < impl_->policy.maximum_connect_attempts)
            std::this_thread::sleep_for(impl_->policy.retry_delay * (attempt + 1));
    }
    if (!result.ok())
        return result;
    {
        std::lock_guard lock(impl_->gate);
        if (cancellation_revision != impl_->cancellation_revision) {
            impl_->transport->close();
            return failure(ResultCode::Canceled, "Connection canceled.");
        }
        impl_->online = true;
        impl_->poisoned = false;
        ++impl_->identity;
        impl_->sequence = 0;
        impl_->family = Family::Unknown;
        impl_->order = 0;
        impl_->last_incoming.reset();
        impl_->pending.reset();
        impl_->decoder.reset();
        impl_->error.clear();
    }
    impl_->receiver = std::thread([self = impl_.get()] { self->receive(); });
    impl_->emit("connected");
    return {};
}
void ProtocolSession::disconnect() noexcept {
    abort();
    std::lock_guard lifecycle(impl_->lifecycle);
    impl_->stop();
}
void ProtocolSession::abort() noexcept {
    {
        std::lock_guard lock(impl_->gate);
        ++impl_->cancellation_revision;
        impl_->online = false;
        impl_->poisoned = true;
    }
    impl_->transport->close();
    impl_->changed.notify_all();
}
bool ProtocolSession::connected() const {
    std::lock_guard lock(impl_->gate);
    return impl_->online && !impl_->poisoned;
}
std::uint64_t ProtocolSession::link() const {
    std::lock_guard lock(impl_->gate);
    return impl_->identity;
}
void ProtocolSession::set_observer(std::function<void(const Observation &)> observer) {
    std::lock_guard lock(impl_->gate);
    impl_->observer = std::move(observer);
}
void ProtocolSession::set_trace(std::function<void(const ProtocolTrace &)> trace) {
    std::lock_guard lock(impl_->gate);
    impl_->trace = std::move(trace);
}
Reply ProtocolSession::exchange(const Query &query) {
    if (!query.to_frame(0).requires_acknowledgement() || query.payload.size() < 2)
        return {failure(ResultCode::InvalidValue, "A discriminated MDR table request is required.")};
    std::unique_lock operation(impl_->operations, std::try_to_lock);
    if (!operation.owns_lock())
        return {failure(ResultCode::Busy, "A protocol request is already running.")};
    Frame sent;
    std::uint64_t link;
    {
        std::lock_guard lock(impl_->gate);
        if (!impl_->online || impl_->poisoned)
            return {failure(ResultCode::NotConnected, "Reconnect the MDR control channel.")};
        link = impl_->identity;
        sent = query.to_frame(impl_->sequence);
        impl_->pending = Impl::Pending{query, impl_->sequence};
    }
    try {
        for (unsigned attempt = 0;
             attempt < (query.await_response ? impl_->policy.maximum_query_attempts : 1); ++attempt) {
            const auto deadline = std::chrono::steady_clock::now() + impl_->policy.request_timeout;
            impl_->write(sent);
            impl_->emit(attempt ? "query-retry" : "send", sent);
            std::unique_lock lock(impl_->gate);
            const auto completed = impl_->changed.wait_until(lock, deadline, [&] {
                return !impl_->online || impl_->identity != link ||
                       (impl_->pending && impl_->pending->ack &&
                        (!query.await_response || impl_->pending->response));
            });
            if (!impl_->online || impl_->identity != link) {
                impl_->pending.reset();
                return {failure(ResultCode::NotConnected,
                                impl_->error.empty() ? "Control channel changed." : impl_->error)};
            }
            if (completed) {
                Reply reply{
                    {query.await_response ? ResultCode::Success : ResultCode::Delivered,
                     query.await_response ? "" : "Command delivered; device state needs confirmation."},
                    impl_->pending->response,
                    impl_->pending->order};
                impl_->sequence = static_cast<Byte>(1 - sent.sequence);
                impl_->pending.reset();
                return reply;
            }
            // Retry a read only when nothing acknowledges delivery. A response or
            // an ACK followed by silence is ambiguous and must quarantine the link.
            if (impl_->pending->ack || impl_->pending->response ||
                attempt + 1 >= impl_->policy.maximum_query_attempts || !query.await_response)
                break;
        }
        {
            std::lock_guard lock(impl_->gate);
            impl_->pending.reset();
        }
        impl_->emit("query-timeout", sent);
        impl_->fail("ACK or response timeout; late replies quarantined until reconnect.");
        return {failure(ResultCode::Timeout,
                        "ACK or device response timed out; reconnect to isolate late replies.")};
    } catch (const std::exception &error) {
        {
            std::lock_guard lock(impl_->gate);
            impl_->pending.reset();
        }
        impl_->fail(error.what());
        return {failure(ResultCode::Transport, error.what())};
    }
}

struct DeviceController::Impl {
    ProtocolSession session;
    mutable std::mutex gate;
    std::mutex operations;
    std::condition_variable alerts_changed;
    std::condition_variable recovery_changed;
    std::condition_variable safe_listening_changed;
    Milliseconds mode_confirmation_timeout;
    std::chrono::steady_clock::time_point next_pressure_read{};
    std::thread recovery;
    std::atomic<bool> stopping{false};
    bool desired_connected{}, system_suspended{}, radio_available{true}, recovery_in_progress{};
    unsigned recovery_budget{};
    std::uint64_t connection_intent{}, codec_generation{};
    bool codec_needs_query{true};
    struct CodecQueryContext {
        std::string source;
        std::uint64_t link{}, generation{};
    };
    struct BufferedCodecNotification {
        Message message;
        std::uint64_t order{};
        CodecQueryContext context;
    };
    std::optional<CodecQueryContext> codec_query_in_flight;
    std::optional<BufferedCodecNotification> buffered_codec_notification;
    DeviceSnapshot state;
    std::optional<SoundParameter> sound;
    std::optional<EqualizerCapability> equalizer_cap;
    std::optional<EqualizerParameter> equalizer;
    std::map<Feature, ExtendedProfile> profiles;
    std::map<Feature, Byte> last_selected;
    std::map<std::string, std::uint64_t> field_orders;
    std::optional<Byte> multipoint_slot, source_type, touch_type, guidance_type, guidance_language, cycle_key;
    std::optional<SourceCapability> source_cap;
    std::optional<SourceSwitchResult> switch_result;
    std::uint64_t switch_result_order{}, fixation_result_order{}, alert_order{};
    std::optional<Byte> fixation_result;
    std::uint64_t fixation_generation{};
    std::optional<std::pair<std::uint64_t, std::uint64_t>> fixation_query;
    Bytes key_availability;
    Byte limitation{}, assistant_key_type{0xff};
    bool ambiguous_multipoint{}, source_switch_reported{}, codec_reported{}, fixation_reported{},
        volume_reported{}, cycle_reported{}, cycle_available{}, cycle_details{}, guidance_available{},
        guidance_switch_reported{}, alerts_registered{}, voice_assistant_reported{};
    Impl(std::unique_ptr<ByteTransport> transport, SessionPolicy policy)
        : session(std::move(transport), policy), mode_confirmation_timeout(policy.request_timeout) {
        state.status = "Disconnected";
        session.set_observer([this](const Observation &value) {
            std::lock_guard lock(gate);
            if (state.link != value.link)
                return;
            apply(value.message, value.receive_order);
        });
        recovery = std::thread([this] { maintain(); });
    }
    ~Impl() {
        stopping = true;
        recovery_changed.notify_all();
        safe_listening_changed.notify_all();
        session.disconnect();
        if (recovery.joinable())
            recovery.join();
        session.set_observer({});
        session.disconnect();
    }
    void reset_for_link(const DeviceDescriptor &device) {
        const auto revision = state.revision;
        const auto last_result = state.last_result;
        state = {};
        state.revision = revision + 1;
        state.busy = true;
        state.device = device;
        state.last_result = last_result;
        state.status = "Connecting Sony MDR control service";
        sound.reset();
        equalizer.reset();
        equalizer_cap.reset();
        source_cap.reset();
        field_orders.clear();
        key_availability.clear();
        touch_type.reset();
        multipoint_slot.reset();
        state.pending_alerts.clear();
        alert_order = 0;
        switch_result.reset();
        switch_result_order = fixation_result_order = 0;
        ++fixation_generation;
        fixation_query.reset();
        profiles.clear();
        last_selected.clear();
        cycle_key.reset();
        guidance_language.reset();
        guidance_switch_reported = guidance_available = false;
        codec_reported = false;
        codec_needs_query = true;
        ++codec_generation;
        codec_query_in_flight.reset();
        buffered_codec_notification.reset();
        next_pressure_read = {};
    }
    void maintain() {
        while (!stopping) {
            {
                std::unique_lock lock(gate);
                recovery_changed.wait_for(lock, Milliseconds{100}, [&] { return stopping.load(); });
                if (stopping)
                    return;
            }
            // Selection and completion belong to the same operation. A manual
            // connection cannot finish between capturing the old device and
            // acquiring the operation lock, or before publishing recovery.
            std::unique_lock operation_lock(operations, std::try_to_lock);
            if (!operation_lock.owns_lock())
                continue;
            std::optional<DeviceDescriptor> selection;
            Result prior;
            std::uint64_t intent{}, previous_link{};
            {
                std::lock_guard lock(gate);
                if (!desired_connected || system_suspended || !radio_available || state.busy ||
                    recovery_in_progress || !recovery_budget || !state.device || session.connected())
                    continue;
                selection = state.device;
                intent = connection_intent;
                previous_link = state.link;
                prior = state.last_result;
                recovery_in_progress = true;
                --recovery_budget;
                state.ready = state.connected = false;
                state.status = "Reconnecting control channel; previous setting commands will not be replayed";
                access();
                ++state.revision;
            }
            const auto result = command_locked(
                [&] {
                    {
                        std::lock_guard lock(gate);
                        if (intent != connection_intent || state.link != previous_link || !state.device ||
                            state.device->address != selection->address || !desired_connected ||
                            system_suspended || !radio_available || stopping)
                            return failure(ResultCode::Canceled, "Automatic recovery canceled.");
                    }
                    session.disconnect();
                    {
                        std::lock_guard lock(gate);
                        if (intent != connection_intent || !desired_connected || system_suspended ||
                            !radio_available || stopping)
                            return failure(ResultCode::Canceled, "Automatic recovery canceled.");
                        reset_for_link(*selection);
                    }
                    auto connected = session.connect(*selection);
                    if (!connected.ok())
                        return connected;
                    bool canceled;
                    {
                        std::lock_guard lock(gate);
                        canceled = intent != connection_intent || !desired_connected || system_suspended ||
                                   !radio_available || stopping;
                        if (!canceled) {
                            state.link = session.link();
                            state.connected = true;
                        }
                    }
                    if (canceled) {
                        session.disconnect();
                        return failure(ResultCode::Canceled, "Automatic recovery canceled.");
                    }
                    connected = bootstrap(intent);
                    if (!connected.ok())
                        session.disconnect();
                    return connected;
                },
                intent);
            {
                std::lock_guard lock(gate);
                recovery_in_progress = false;
                if (intent != connection_intent)
                    continue;
                if (result.code == ResultCode::Busy)
                    ++recovery_budget;
                else if (result.ok()) {
                    state.status =
                        "Reconnected and read actual state; previous setting commands were not replayed";
                    if (!prior.ok())
                        state.last_result = prior;
                } else if (result.code != ResultCode::Canceled)
                    state.status =
                        "Automatic recovery reached its attempt limit; reconnect manually. " + result.message;
                ++state.revision;
            }
        }
    }
    bool model_checked() const { return state.model == "WF-1000XM5" && state.ready; }
    bool can_assign() const {
        const auto &touch = state.touch;
        return touch.assignments.known() && touch.keys.size() == key_availability.size() &&
               !touch.keys.empty() && touch.assignments.value->size() == touch.keys.size() &&
               limitation <= 2 &&
               std::all_of(touch.keys.begin(), touch.keys.end(),
                           [](const auto &key) { return key.id <= 5 && key.type <= 2; }) &&
               std::all_of(touch.assignments.value->begin(), touch.assignments.value->end(),
                           InteractionParameters::known_preset);
    }
    const TouchPreset *active_preset(Byte key) const {
        const auto &touch = state.touch;
        if (!touch.assignments.known())
            return nullptr;
        for (std::size_t i = 0; i < touch.keys.size() && i < touch.assignments.value->size(); ++i)
            if (touch.keys[i].id == key) {
                const auto found =
                    std::find_if(touch.keys[i].presets.begin(), touch.keys[i].presets.end(),
                                 [&](const auto &p) { return p.id == (*touch.assignments.value)[i]; });
                return found == touch.keys[i].presets.end() ? nullptr : &*found;
            }
        return nullptr;
    }
    bool cycle_dependency() const {
        if (!can_assign() || !cycle_key)
            return false;
        const auto &keys = state.touch.keys;
        const auto key =
            std::find_if(keys.begin(), keys.end(), [&](const auto &k) { return k.id == *cycle_key; });
        if (key == keys.end() || key_availability[static_cast<std::size_t>(key - keys.begin())] != 0)
            return false;
        const auto preset = active_preset(*cycle_key);
        if (!preset)
            return false;
        if (!cycle_details)
            return std::any_of(preset->functions.begin(), preset->functions.end(),
                               [](auto f) { return f >= 1 && f <= 4; });
        if (!state.touch.details.known() || !state.touch.cycle.known())
            return false;
        return std::any_of(preset->customizable.begin(), preset->customizable.end(), [&](const auto &a) {
            return a.action == 0 &&
                   std::find(a.functions.begin(), a.functions.end(),
                             static_cast<Byte>(*state.touch.cycle.value)) != a.functions.end();
        });
    }
    void derive_cycle(std::uint64_t order, bool notification) {
        if (!cycle_details || !cycle_key)
            return;
        const auto preset = active_preset(*cycle_key);
        if (!preset || !state.touch.details.known()) {
            unknown(state.touch.cycle, "Ambient cycle action has not been read.", order);
            return;
        }
        for (const auto &value : *state.touch.details.value)
            if (value.preset == preset->id)
                for (const auto &action : value.actions)
                    if (action.action == 0 && action.function >= 1 && action.function <= 4) {
                        observe(state.touch.cycle, static_cast<int>(action.function), order, notification);
                        return;
                    }
        unknown(state.touch.cycle, "Current touch assignment has no editable ambient cycle.", order);
    }
    void access() {
        const bool checked = model_checked();
        auto &sl = state.safe_listening;
        sl.writable = state.connected && state.ready && sl.type && sl.available &&
                      sl.minimum_interval_seconds && *sl.minimum_interval_seconds > 0;
        auto &eq = state.equalizer;
        eq.five_band_layout = equalizer_cap && equalizer_cap->band_count == 6 &&
                              equalizer_cap->step_count == 21 && equalizer &&
                              (equalizer->steps.empty() || equalizer->five_bands());
        eq.writable = checked && eq.available && eq.five_band_layout && equalizer_cap &&
                      std::any_of(equalizer_cap->presets.begin(), equalizer_cap->presets.end(),
                                  [&](const auto &p) { return equalizer && p.id == equalizer->preset_id; });
        eq.reason = eq.writable
                        ? "Reported six-value EQ layout; hardware validation is pending."
                        : "Model, availability, preset or six-value/21-step EQ layout is not confirmed.";
        state.sound.writable =
            checked && state.sound.available && sound && sound->settled && !state.sound.ranges.empty();
        state.sources.multipoint_writable = checked && multipoint_slot && !ambiguous_multipoint &&
                                            state.features[Feature::Multipoint].available &&
                                            state.sources.multipoint.known();
        state.sources.switch_writable =
            checked && source_switch_reported && state.sources.available && source_cap &&
            source_cap->maximum_connected > 0 && state.sources.multipoint.known() &&
            *state.sources.multipoint.value && state.sources.active_source.known() &&
            (!fixation_reported || state.touch.playback_fixed.known() && !*state.touch.playback_fixed.value);
        state.touch.writable =
            checked && can_assign() &&
            std::any_of(key_availability.begin(), key_availability.end(), [](auto v) { return v == 0; });
        state.touch.cycle_writable =
            checked && cycle_reported && cycle_available && state.touch.cycle.known() && cycle_dependency();
        state.touch.guidance_writable =
            checked && guidance_type && guidance_switch_reported && guidance_available &&
            state.touch.guidance.known() &&
            ((*guidance_type != 1 && *guidance_type != 2) ||
             (guidance_language && *guidance_language != 0 && *guidance_language != 0xff));
        state.touch.volume_writable = checked && volume_reported && state.touch.prompt_volume.known() &&
                                      guidance_available && state.touch.guidance.known() &&
                                      *state.touch.guidance.value == 0;
        state.touch.fixation_writable = checked && fixation_reported && state.sources.available &&
                                        state.sources.multipoint.known() && *state.sources.multipoint.value &&
                                        state.sources.active_source.known() &&
                                        state.touch.playback_fixed.known();
        state.touch.assistant_writable = checked && voice_assistant_reported &&
                                         state.touch.voice_assistant.known() &&
                                         !state.touch.voice_assistant_candidates.empty();
        for (auto &[feature, item] : state.features) {
            const auto profile = profiles.find(feature);
            if (profile != profiles.end()) {
                item.writable = checked && item.available && item.value.known() && !item.choices.empty();
                if (feature == Feature::AutoPowerOff || feature == Feature::SpeakToChat)
                    item.writable &= last_selected.contains(feature) &&
                                     ExtendedParameters::legal(profile->second, last_selected[feature]);
                if (profile->second.function == 0xe7)
                    item.writable &= last_selected.contains(feature) && last_selected[feature] == 0;
                if (feature == Feature::PowerOff)
                    item.writable = checked && item.advertised;
            }
        }
        auto set_access = [&](Feature f, bool writable, bool available) {
            auto &feature = state.features[f];
            feature.writable = writable;
            feature.available = available;
            auto &capability = state.capabilities.features[f];
            capability.access.write = writable             ? Support::Supported
                                      : feature.advertised ? Support::Unknown
                                                           : Support::Unsupported;
            capability.hardware_verified = false;
        };
        set_access(Feature::SoundMode, state.sound.writable, state.sound.available);
        set_access(Feature::AmbientLevel, state.sound.writable, state.sound.available);
        set_access(Feature::VoiceFocus, state.sound.writable, state.sound.available);
        set_access(Feature::Equalizer, eq.writable, eq.available);
        set_access(Feature::ClearBass, eq.writable, eq.available);
        set_access(Feature::Multipoint, state.sources.multipoint_writable,
                   state.features[Feature::Multipoint].available);
        set_access(Feature::SourceSwitch, state.sources.switch_writable, state.sources.available);
        set_access(Feature::TouchAssignment, state.touch.writable, state.touch.writable);
        set_access(Feature::AmbientCycle, state.touch.cycle_writable, cycle_available);
        set_access(Feature::Notifications, state.touch.guidance_writable, guidance_available);
        set_access(Feature::PromptVolume, state.touch.volume_writable, state.touch.prompt_volume.known());
        set_access(Feature::FixedPlayback, state.touch.fixation_writable, state.touch.playback_fixed.known());
        set_access(Feature::SoundPressure, sl.writable, sl.available);
        for (const auto &[f, profile] : profiles) {
            (void)profile;
            state.capabilities.features[f].access.write =
                state.features[f].writable ? Support::Supported : Support::Unknown;
        }
    }
    void invalidate_codec(std::string reason) {
        codec_needs_query = true;
        ++codec_generation;
        codec_query_in_flight.reset();
        buffered_codec_notification.reset();
        unknown(state.codec, std::move(reason));
    }
    bool current_codec_query(const CodecQueryContext &context) const {
        return context.link == state.link && context.generation == codec_generation &&
               state.sources.active_source.known() && context.source == *state.sources.active_source.value;
    }
    void apply(const Message &message, std::uint64_t order,
               std::optional<CodecQueryContext> codec_query = {}) {
        const auto &info = message.information;
        const bool notification = message.kind == Kind::Notification;
        if (const auto cap = std::get_if<SafeListeningCapability>(&info)) {
            auto &sl = state.safe_listening;
            if (sl.type == cap->type) {
                sl.minimum_interval_seconds = cap->minimum_interval;
                sl.reason = cap->minimum_interval ? "" : "Device reported a zero polling interval.";
            }
        } else if (const auto available = std::get_if<SafeListeningAvailability>(&info)) {
            auto &sl = state.safe_listening;
            if (sl.type == available->type) {
                sl.available = available->available;
                if (!sl.available) {
                    unknown(sl.decibels, "Safe Listening is unavailable.", order);
                    sl.updated_at.reset();
                    sl.reason = "Safe Listening is unavailable on the device.";
                }
            }
        } else if (const auto mode = std::get_if<SafeListeningMode>(&info)) {
            auto &sl = state.safe_listening;
            if (sl.type == mode->type && order > sl.enabled.receive_order) {
                const bool changed = !sl.enabled.known() || *sl.enabled.value != mode->enabled ||
                                     !sl.preview.known() || *sl.preview.value != mode->preview;
                observe(sl.enabled, mode->enabled, order, notification);
                observe(sl.preview, mode->preview, order, notification);
                if (changed || !mode->enabled) {
                    unknown(sl.decibels, "Waiting for a current sound pressure reading.", order);
                    unknown(sl.cause, "Waiting for a current sound pressure reading.", order);
                    sl.updated_at.reset();
                    next_pressure_read = {};
                }
                safe_listening_changed.notify_all();
            }
        } else if (const auto pressure = std::get_if<SoundPressure>(&info)) {
            auto &sl = state.safe_listening;
            if (sl.type == pressure->type && sl.enabled.known() && *sl.enabled.value &&
                order > sl.cause.receive_order) {
                observe(sl.cause, static_cast<int>(pressure->cause), order, notification);
                if (pressure->valid_level())
                    observe(sl.decibels, static_cast<int>(pressure->level), order, notification);
                else
                    unknown(sl.decibels, "No valid current sound pressure measurement.", order);
                sl.updated_at = std::chrono::steady_clock::now();
                sl.reason.clear();
            }
        } else if (const auto parsedBatteryStatus = std::get_if<BatteryStatus>(&info)) {
            auto unit = [&](const BatteryUnit &source, Value<int> &level, Value<bool> &charging) {
                if (source.level <= 100)
                    observe(level, static_cast<int>(source.level), order, notification);
                else
                    unknown(level, "Battery value is not a percentage.", order);
                if (source.charging == 0 || source.charging == 1 || source.charging == 3)
                    observe(charging, source.charging == 1, order, notification);
                else
                    unknown(charging, "Unknown charging state.", order);
            };
            const bool wf = state.model == "WF-1000XM5", wh = state.model == "WH-1000XM5";
            if (wh && (parsedBatteryStatus->inquired_type == 0 || parsedBatteryStatus->inquired_type == 8))
                unit(parsedBatteryStatus->first, state.battery.single, state.battery.single_charging);
            if (wf && (parsedBatteryStatus->inquired_type == 1 || parsedBatteryStatus->inquired_type == 9) &&
                parsedBatteryStatus->second) {
                unit(parsedBatteryStatus->first, state.battery.left, state.battery.left_charging);
                unit(*parsedBatteryStatus->second, state.battery.right, state.battery.right_charging);
            }
            if (wf && (parsedBatteryStatus->inquired_type == 2 || parsedBatteryStatus->inquired_type == 10))
                unit(parsedBatteryStatus->first, state.battery.case_level, state.battery.case_charging);
        } else if (const auto parsedSoundCapability = std::get_if<SoundCapability>(&info)) {
            state.sound.ranges = parsedSoundCapability->ranges;
        } else if (const auto parsedSoundAvailability = std::get_if<SoundAvailability>(&info)) {
            if (order > field_orders["sound-status"]) {
                field_orders["sound-status"] = order;
                state.sound.available = parsedSoundAvailability->available;
            }
        } else if (const auto parsedSoundParameter = std::get_if<SoundParameter>(&info)) {
            if (order > state.sound.mode.receive_order) {
                sound = *parsedSoundParameter;
                observe(state.sound.mode, parsedSoundParameter->sound_mode(), order, notification);
                observe(state.sound.ambient_level, static_cast<int>(parsedSoundParameter->level), order,
                        notification);
                observe(state.sound.voice_focus, parsedSoundParameter->ambient_mode == 1, order,
                        notification);
            }
        } else if (const auto parsedEqualizerCapability = std::get_if<EqualizerCapability>(&info)) {
            equalizer_cap = *parsedEqualizerCapability;
            state.equalizer.presets = parsedEqualizerCapability->presets;
            state.equalizer.band_count = parsedEqualizerCapability->band_count;
            state.equalizer.step_count = parsedEqualizerCapability->step_count;
        } else if (const auto parsedEqualizerAvailability = std::get_if<EqualizerAvailability>(&info)) {
            if (order > field_orders["eq-status"]) {
                field_orders["eq-status"] = order;
                state.equalizer.available = parsedEqualizerAvailability->available;
            }
        } else if (const auto parsedEqualizerParameter = std::get_if<EqualizerParameter>(&info)) {
            if (order > state.equalizer.preset.receive_order) {
                equalizer = *parsedEqualizerParameter;
                observe(state.equalizer.preset, static_cast<int>(parsedEqualizerParameter->preset_id), order,
                        notification);
                if (parsedEqualizerParameter->five_bands()) {
                    observe(state.equalizer.bands, parsedEqualizerParameter->bands(), order, notification);
                    observe(state.equalizer.clear_bass,
                            static_cast<int>(parsedEqualizerParameter->steps[0]) - 10, order, notification);
                } else {
                    unknown(state.equalizer.bands, "EQ layout has not been confirmed.", order);
                    unknown(state.equalizer.clear_bass, "CLEAR BASS layout has not been confirmed.", order);
                }
            }
        } else if (const auto parsedExtendedValue = std::get_if<ExtendedValue>(&info)) {
            if (parsedExtendedValue->field == ExtendedField::Codec) {
                if (codec_reported && session.connected() && state.sources.active_source.known() &&
                    !parsedExtendedValue->data.empty() && notification && codec_needs_query &&
                    codec_query_in_flight && current_codec_query(*codec_query_in_flight)) {
                    // A notification cannot identify its source. Keep the query
                    // fence until success, but retain a newer receive-order update
                    // in case the response arrived before its request ACK.
                    if (!buffered_codec_notification || order > buffered_codec_notification->order)
                        buffered_codec_notification =
                            BufferedCodecNotification{message, order, *codec_query_in_flight};
                } else if (codec_reported && session.connected() && state.sources.active_source.known() &&
                           !parsedExtendedValue->data.empty() &&
                           (codec_query ? current_codec_query(*codec_query) : !codec_needs_query) &&
                           order > state.codec.receive_order) {
                    codec_needs_query = false;
                    const auto code = parsedExtendedValue->data[0];
                    std::string name = code == 1      ? "SBC"
                                       : code == 2    ? "AAC"
                                       : code == 0x10 ? "LDAC"
                                       : code == 0x20 ? "aptX"
                                       : code == 0x21 ? "aptX HD"
                                       : code == 0x30 ? "LC3"
                                                      : "";
                    if (name.empty())
                        unknown(state.codec, "Codec negotiation is pending or unknown.", order);
                    else
                        observe(state.codec, name, order, notification);
                }
            } else if (profiles.contains(parsedExtendedValue->feature)) {
                const auto profile = profiles[parsedExtendedValue->feature];
                if (profile.group != parsedExtendedValue->group ||
                    parsedExtendedValue->type != (parsedExtendedValue->field == ExtendedField::Availability
                                                      ? profile.status_type()
                                                      : profile.type))
                    return;
                auto &item = state.features[parsedExtendedValue->feature];
                const auto key =
                    "extended:" + std::to_string(static_cast<int>(parsedExtendedValue->feature)) + ":" +
                    std::to_string(static_cast<int>(parsedExtendedValue->field));
                if (order <= field_orders[key])
                    return;
                field_orders[key] = order;
                if (parsedExtendedValue->field == ExtendedField::Capability) {
                    item.choices.clear();
                    if (parsedExtendedValue->feature == Feature::AutoPowerOff || profile.function == 0xe7) {
                        if (!parsedExtendedValue->data.empty() &&
                            parsedExtendedValue->data.size() >= 1 + parsedExtendedValue->data[0])
                            for (std::size_t i = 1; i <= parsedExtendedValue->data[0]; ++i)
                                if (ExtendedParameters::legal(profile, parsedExtendedValue->data[i]))
                                    item.choices.push_back(parsedExtendedValue->data[i]);
                        if (std::find(parsedExtendedValue->data.begin(), parsedExtendedValue->data.end(),
                                      0xff) != parsedExtendedValue->data.end())
                            item.choices.clear();
                    } else if (!(parsedExtendedValue->feature == Feature::Dsee &&
                                 !parsedExtendedValue->data.empty() &&
                                 parsedExtendedValue->data[0] == 0xff) &&
                               !(parsedExtendedValue->feature == Feature::SpeakToChat &&
                                 !parsedExtendedValue->data.empty() && parsedExtendedValue->data[0] == 0x7f))
                        item.choices = {0, 1};
                } else if (parsedExtendedValue->field == ExtendedField::Availability &&
                           !parsedExtendedValue->data.empty())
                    item.available = parsedExtendedValue->data[0] == 0;
                else if (parsedExtendedValue->field == ExtendedField::Parameter &&
                         !parsedExtendedValue->data.empty()) {
                    if (std::find(item.choices.begin(), item.choices.end(), parsedExtendedValue->data[0]) !=
                        item.choices.end())
                        observe(item.value, static_cast<int>(parsedExtendedValue->data[0]), order,
                                notification);
                    else
                        unknown(item.value, "Device returned a sentinel or unadvertised value.", order);
                    if (parsedExtendedValue->feature == Feature::AutoPowerOff ||
                        parsedExtendedValue->feature == Feature::SpeakToChat || profile.function == 0xe7)
                        last_selected[parsedExtendedValue->feature] =
                            parsedExtendedValue->data.size() > 1 ? parsedExtendedValue->data[1] : 0;
                }
            }
        } else if (const auto parsedGeneralSettingCapability = std::get_if<GeneralSettingCapability>(&info)) {
            if (parsedGeneralSettingCapability->multipoint()) {
                if (multipoint_slot && *multipoint_slot != parsedGeneralSettingCapability->slot) {
                    ambiguous_multipoint = true;
                    multipoint_slot.reset();
                } else if (!ambiguous_multipoint)
                    multipoint_slot = parsedGeneralSettingCapability->slot;
            }
        } else if (const auto parsedGeneralSettingAvailability =
                       std::get_if<GeneralSettingAvailability>(&info)) {
            if (multipoint_slot == parsedGeneralSettingAvailability->slot &&
                order > field_orders["multipoint-status"]) {
                field_orders["multipoint-status"] = order;
                state.features[Feature::Multipoint].available = parsedGeneralSettingAvailability->available;
            }
        } else if (const auto parsedGeneralSettingParameter = std::get_if<GeneralSettingParameter>(&info)) {
            if (multipoint_slot == parsedGeneralSettingParameter->slot)
                observe(state.sources.multipoint, parsedGeneralSettingParameter->enabled, order,
                        notification);
        } else if (const auto parsedSourceCapability = std::get_if<SourceCapability>(&info)) {
            if (source_type == parsedSourceCapability->subtype)
                source_cap = *parsedSourceCapability;
        } else if (const auto parsedSourceAvailability = std::get_if<SourceAvailability>(&info)) {
            if (source_type == parsedSourceAvailability->subtype && order > field_orders["source-status"]) {
                field_orders["source-status"] = order;
                state.sources.available = parsedSourceAvailability->available;
            }
        } else if (const auto parsedSourceList = std::get_if<SourceList>(&info)) {
            if (source_type == parsedSourceList->subtype && order > field_orders["sources"]) {
                field_orders["sources"] = order;
                const auto old_source = state.sources.active_source.value;
                if (!source_cap || parsedSourceList->entries.size() > source_cap->maximum_paired ||
                    std::count_if(parsedSourceList->entries.begin(), parsedSourceList->entries.end(),
                                  [](const auto &e) { return e.connection_slot != 0; }) >
                        source_cap->maximum_connected) {
                    state.sources.entries.clear();
                    unknown(state.sources.active_source, "Source list exceeds the reported capability.",
                            order);
                } else {
                    state.sources.entries = parsedSourceList->entries;
                    if (const auto active = parsedSourceList->active_source())
                        observe(state.sources.active_source, *active, order, notification);
                    else
                        unknown(state.sources.active_source, "No unique active playback source.", order);
                }
                if (old_source != state.sources.active_source.value) {
                    ++fixation_generation;
                    invalidate_codec("Source changed; refresh negotiated codec.");
                    unknown(state.touch.playback_fixed, "Source changed; refresh fixation state.");
                }
            }
        } else if (const auto parsedSourceSwitchResult = std::get_if<SourceSwitchResult>(&info)) {
            if (order > switch_result_order) {
                switch_result = *parsedSourceSwitchResult;
                switch_result_order = order;
            }
        } else if (const auto parsedMultipointAlert = std::get_if<MultipointAlert>(&info)) {
            if (state.pending_alerts.size() == 32)
                state.pending_alerts.erase(state.pending_alerts.begin());
            state.pending_alerts.push_back(*parsedMultipointAlert);
            alert_order = order;
            alerts_changed.notify_all();
        } else if (const auto parsedTouchCapability = std::get_if<TouchCapability>(&info)) {
            if (touch_type == parsedTouchCapability->type) {
                state.touch.keys = parsedTouchCapability->keys;
                limitation = parsedTouchCapability->limitation;
            }
        } else if (const auto parsedTouchStatus = std::get_if<TouchStatus>(&info)) {
            if (touch_type == parsedTouchStatus->type && order > field_orders["touch-status"]) {
                field_orders["touch-status"] = order;
                key_availability = parsedTouchStatus->values;
            }
        } else if (const auto parsedTouchSelection = std::get_if<TouchSelection>(&info)) {
            if (touch_type == parsedTouchSelection->type) {
                bool legal = parsedTouchSelection->values.size() == state.touch.keys.size();
                for (std::size_t i = 0; legal && i < parsedTouchSelection->values.size(); ++i)
                    legal = std::any_of(
                        state.touch.keys[i].presets.begin(), state.touch.keys[i].presets.end(),
                        [&](const auto &preset) { return preset.id == parsedTouchSelection->values[i]; });
                if (legal)
                    observe(state.touch.assignments, parsedTouchSelection->values, order, notification);
                else
                    unknown(state.touch.assignments,
                            "Assignments do not match the advertised key/preset layout.", order);
                derive_cycle(order, notification);
            }
        } else if (const auto parsedTouchDetails = std::get_if<TouchDetails>(&info)) {
            if (touch_type == parsedTouchDetails->type) {
                const bool legal = std::all_of(
                    parsedTouchDetails->values.begin(), parsedTouchDetails->values.end(),
                    [&](const auto &selected) {
                        return std::any_of(
                            state.touch.keys.begin(), state.touch.keys.end(), [&](const auto &key) {
                                return std::any_of(
                                    key.presets.begin(), key.presets.end(), [&](const auto &preset) {
                                        return preset.id == selected.preset &&
                                               std::all_of(
                                                   selected.actions.begin(), selected.actions.end(),
                                                   [&](const auto &action) {
                                                       return std::find(preset.actions.begin(),
                                                                        preset.actions.end(),
                                                                        action) != preset.actions.end() ||
                                                              std::any_of(
                                                                  preset.customizable.begin(),
                                                                  preset.customizable.end(),
                                                                  [&](const auto &custom) {
                                                                      return custom.action == action.action &&
                                                                             std::find(
                                                                                 custom.functions.begin(),
                                                                                 custom.functions.end(),
                                                                                 action.function) !=
                                                                                 custom.functions.end();
                                                                  });
                                                   });
                                    });
                            });
                    });
                if (legal)
                    observe(state.touch.details, parsedTouchDetails->values, order, notification);
                else
                    unknown(state.touch.details, "Touch action values do not match advertised candidates.",
                            order);
                derive_cycle(order, notification);
            }
        } else if (const auto parsedCycleValue = std::get_if<CycleValue>(&info)) {
            if (cycle_reported) {
                if (parsedCycleValue->command == 0x61)
                    cycle_key = parsedCycleValue->value <= 5 ? std::optional<Byte>{parsedCycleValue->value}
                                                             : std::nullopt;
                else if (parsedCycleValue->command == 0x63 || parsedCycleValue->command == 0x65) {
                    if (order > field_orders["cycle-status"]) {
                        field_orders["cycle-status"] = order;
                        cycle_available = parsedCycleValue->value == 0;
                    }
                } else if (parsedCycleValue->command == 0x67 || parsedCycleValue->command == 0x69) {
                    if (parsedCycleValue->value >= 1 && parsedCycleValue->value <= 4)
                        observe(state.touch.cycle, static_cast<int>(parsedCycleValue->value), order,
                                notification);
                    else
                        unknown(state.touch.cycle, "Unknown ambient cycle.", order);
                }
            }
        } else if (const auto parsedGuidanceValue = std::get_if<GuidanceValue>(&info)) {
            if (guidance_type == parsedGuidanceValue->type && !parsedGuidanceValue->data.empty()) {
                if (parsedGuidanceValue->command == 0x41) {
                    // C# and the pinned MIT capability schema use byte 6 for MTK types 0/1,
                    // byte 2 for type 2. Never infer switching support from a readable value.
                    const auto index = parsedGuidanceValue->type <= 1 ? 4u : 0u;
                    if (parsedGuidanceValue->data.size() > index &&
                        order > field_orders["guidance-capability"]) {
                        field_orders["guidance-capability"] = order;
                        guidance_switch_reported = parsedGuidanceValue->data[index] == 1;
                    }
                } else if (parsedGuidanceValue->command == 0x43 || parsedGuidanceValue->command == 0x45) {
                    if (order > field_orders["guidance-status"]) {
                        field_orders["guidance-status"] = order;
                        guidance_available = parsedGuidanceValue->data.back() == 0;
                    }
                } else if ((parsedGuidanceValue->command == 0x47 || parsedGuidanceValue->command == 0x49) &&
                           order > state.touch.guidance.receive_order) {
                    if (parsedGuidanceValue->data[0] <= 1) {
                        observe(state.touch.guidance, static_cast<int>(parsedGuidanceValue->data[0]), order,
                                notification);
                        if (parsedGuidanceValue->type == 1 || parsedGuidanceValue->type == 2)
                            guidance_language = parsedGuidanceValue->data.size() > 1
                                                    ? std::optional<Byte>{parsedGuidanceValue->data[1]}
                                                    : std::nullopt;
                    } else
                        unknown(state.touch.guidance, "Unknown voice guidance state.", order);
                }
            } else if (volume_reported && parsedGuidanceValue->type == 0x20 &&
                       (parsedGuidanceValue->command == 0x47 || parsedGuidanceValue->command == 0x49) &&
                       !parsedGuidanceValue->data.empty())
                observe(state.touch.prompt_volume,
                        static_cast<int>(static_cast<std::int8_t>(parsedGuidanceValue->data[0])), order,
                        notification);
        } else if (const auto parsedFixValue = std::get_if<FixValue>(&info)) {
            if (fixation_reported && (!fixation_query ||
                (fixation_query->first == state.link && fixation_query->second == fixation_generation))) {
                observe(state.touch.playback_fixed, parsedFixValue->fixed, order, notification);
                if (parsedFixValue->result && order > fixation_result_order) {
                    fixation_result = *parsedFixValue->result;
                    fixation_result_order = order;
                }
            }
        } else if (const auto parsedVoiceAssistantCapability = std::get_if<VoiceAssistantCapability>(&info)) {
            if (voice_assistant_reported) {
                assistant_key_type = parsedVoiceAssistantCapability->key_type;
                state.touch.voice_assistant_candidates = parsedVoiceAssistantCapability->candidates;
            }
        } else if (const auto parsedVoiceAssistantValue = std::get_if<VoiceAssistantValue>(&info)) {
            if (voice_assistant_reported &&
                InteractionParameters::known_voice_assistant(parsedVoiceAssistantValue->value) &&
                std::find(state.touch.voice_assistant_candidates.begin(),
                          state.touch.voice_assistant_candidates.end(),
                          parsedVoiceAssistantValue->value) != state.touch.voice_assistant_candidates.end())
                observe(state.touch.voice_assistant, static_cast<int>(parsedVoiceAssistantValue->value),
                        order, notification);
            else
                unknown(state.touch.voice_assistant, "Unadvertised voice assistant value.", order);
        }
        access();
        ++state.revision;
    }
    Result query(const Query &query) {
        const auto reply = session.exchange(query);
        if (!reply.result.ok())
            return reply.result;
        if (!query.await_response)
            return reply.result;
        if (!reply.response)
            return failure(ResultCode::Protocol, "Device response was missing.");
        const auto parsed = parse(*reply.response, Family::V2);
        if (!parsed.valid())
            return failure(ResultCode::Protocol, "Malformed device response.");
        std::lock_guard lock(gate);
        if (state.link != session.link())
            return failure(ResultCode::Superseded, "Control channel changed.");
        apply(parsed.message, reply.receive_order);
        return {};
    }
    Result confirm(const Query &write, const Query &read, const std::function<bool()> &matches) {
        auto result = query(write);
        if (!result.ok())
            return result;
        result = query(read);
        if (!result.ok())
            return result;
        std::lock_guard lock(gate);
        return matches()
                   ? Result{}
                   : failure(
                         ResultCode::DeviceRejected,
                         "Readback did not confirm the requested value; actual device state is retained.");
    }
    std::optional<MultipointAlert> wait_alert(std::uint64_t baseline) {
        std::unique_lock lock(gate);
        alerts_changed.wait_for(lock, Milliseconds{500},
                                [&] { return alert_order > baseline || !state.connected; });
        if (alert_order > baseline && !state.pending_alerts.empty())
            return state.pending_alerts.back();
        return {};
    }
    template <class F> Result command(F &&operation) {
        std::unique_lock lock(operations, std::try_to_lock);
        if (!lock.owns_lock())
            return failure(ResultCode::Busy, "Another device operation is running.");
        return command_locked(std::forward<F>(operation));
    }
    template <class F>
    Result command_locked(F &&operation, std::optional<std::uint64_t> expected_intent = {}) {
        {
            std::lock_guard state_lock(gate);
            if (expected_intent && *expected_intent != connection_intent) {
                state.busy = false;
                ++state.revision;
                return failure(ResultCode::Canceled, "Automatic recovery superseded.");
            }
            state.busy = true;
            ++state.revision;
        }
        Result result;
        try {
            result = operation();
        } catch (const std::invalid_argument &error) {
            result = failure(ResultCode::InvalidValue, error.what());
        } catch (const std::out_of_range &error) {
            result = failure(ResultCode::InvalidValue, error.what());
        } catch (const std::exception &error) {
            result = failure(ResultCode::Protocol, error.what());
        }
        {
            std::lock_guard state_lock(gate);
            if (expected_intent && *expected_intent != connection_intent) {
                state.busy = false;
                ++state.revision;
                return failure(ResultCode::Canceled, "Automatic recovery superseded.");
            }
            state.connected = session.connected();
            if (!state.connected)
                state.ready = false;
            state.busy = false;
            state.last_result = result;
            if (!result.ok())
                state.status = result.message;
            access();
            ++state.revision;
        }
        return result;
    }
    Result bootstrap(std::optional<std::uint64_t> expected_intent = {}) {
        Handshake handshake(session.link());
        while (const auto request = handshake.current_query()) {
            {
                std::lock_guard lock(gate);
                if (stopping || system_suspended || !radio_available || !desired_connected ||
                    (expected_intent && *expected_intent != connection_intent))
                    return failure(ResultCode::Canceled, "Handshake canceled by connection environment.");
            }
            auto reply = session.exchange(*request);
            if (!reply.result.ok())
                return reply.result;
            if (!reply.response || !handshake.receive(session.link(), *reply.response).applied)
                return failure(ResultCode::Protocol, "Handshake response did not advance the current stage.");
        }
        if (handshake.state().phase != Phase::Ready)
            return failure(ResultCode::Unsupported, handshake.state().error);
        const auto &hs = handshake.state();
        const auto t1 = hs.table1_functions.value_or(SupportFunctions{Table::Table1, {}});
        const auto t2 = hs.table2_functions.value_or(SupportFunctions{Table::Table2, {}});
        std::vector<Query> queries;
        {
            std::lock_guard lock(gate);
            if (expected_intent && *expected_intent != connection_intent)
                return failure(ResultCode::Canceled, "Automatic recovery superseded.");
            state.model = hs.model_name.value_or("");
            state.firmware = hs.firmware.value_or("");
            state.family = hs.protocol->family;
            state.capabilities = map_capabilities(hs);
            const auto sl_type = SafeListeningParameters::select_type(t2);
            if (state.safe_listening.type != sl_type)
                state.safe_listening = {};
            state.safe_listening.type = sl_type;
            if (!sl_type)
                state.safe_listening.reason = "No unambiguous Safe Listening capability was advertised.";
            else {
                state.safe_listening.available = false;
                state.safe_listening.minimum_interval_seconds.reset();
                queries.push_back(SafeListeningParameters::capability(*sl_type));
                queries.push_back(SafeListeningParameters::availability(*sl_type));
            }
            for (const auto &[feature, capability] : state.capabilities.features) {
                auto &item = state.features[feature];
                item.advertised = capability.access.read == Support::Supported;
                item.reason = capability.reason;
            }
            profiles.clear();
            last_selected.clear();
            for (const auto &profile : ExtendedParameters::profiles())
                if (t1.contains(profile.function))
                    profiles[profile.feature] = profile;
            for (const auto &[feature, profile] : profiles) {
                state.features[feature].advertised = true;
                if (feature == Feature::WearDetectionPause) {
                    state.features[feature].choices = {0, 1};
                    state.features[feature].available = true;
                }
                if (feature == Feature::PowerOff) {
                    state.features[feature].choices = {1};
                    state.features[feature].available = true;
                    state.features[feature].reason =
                        "Power-off is an action; ACK/disconnect cannot confirm hardware power state.";
                }
                const auto list = ExtendedParameters::read_queries(profile);
                queries.insert(queries.end(), list.begin(), list.end());
            }
            const auto batteries =
                BatteryParameters::queries(&t1, state.model == "WF-1000XM5"   ? BatteryLayout::LeftRightCase
                                                : state.model == "WH-1000XM5" ? BatteryLayout::Single
                                                                              : BatteryLayout::Unknown);
            queries.insert(queries.end(), batteries.begin(), batteries.end());
            if (state.model == "WF-1000XM5" && t1.contains(0x6b))
                queries.insert(queries.end(), {SoundParameters::capability(), SoundParameters::availability(),
                                               SoundParameters::read()});
            if (t1.contains(0x50))
                queries.insert(queries.end(),
                               {EqualizerParameters::capability(), EqualizerParameters::availability(),
                                EqualizerParameters::read()});
            multipoint_slot.reset();
            ambiguous_multipoint = false;
            for (const auto &function : t1.functions)
                if (function.id >= 0xd1 && function.id <= 0xd4)
                    queries.push_back(AudioSourceParameters::general_capability(function.id));
            source_type = t2.contains(0x32) || t2.contains(0x33) ? std::optional<Byte>{Byte{2}}
                          : t2.contains(0x30)                    ? std::optional<Byte>{Byte{0}}
                                                                 : std::nullopt;
            source_switch_reported = t2.contains(0x31);
            codec_reported = t1.contains(0x12);
            voice_assistant_reported = t1.contains(0xf4);
            touch_type = t1.contains(0xfe)   ? std::optional<Byte>{Byte{0x0e}}
                         : t1.contains(0xf3) ? std::optional<Byte>{Byte{3}}
                                             : std::nullopt;
            cycle_reported = t1.contains(0x69);
            cycle_details = false;
            cycle_key.reset();
            guidance_type = t2.contains(0x42) || t2.contains(0x41) ? std::optional<Byte>{Byte{1}}
                            : t2.contains(0x40)                    ? std::optional<Byte>{Byte{0}}
                            : t2.contains(0x44)                    ? std::optional<Byte>{Byte{2}}
                            : t2.contains(0x45)                    ? std::optional<Byte>{Byte{3}}
                                                                   : std::nullopt;
            guidance_switch_reported = guidance_type == 3;
            volume_reported = t2.contains(0x42) || t2.contains(0x43);
            fixation_reported = t2.contains(0x31) && source_type.has_value();
            if (touch_type)
                queries.insert(queries.end(), {read_query(table1_type, 0xf0, *touch_type),
                                               read_query(table1_type, 0xf2, *touch_type),
                                               InteractionParameters::touch_read(*touch_type)});
            if (cycle_reported)
                queries.insert(queries.end(),
                               {read_query(table1_type, 0x60, 0x30), read_query(table1_type, 0x62, 0x30),
                                InteractionParameters::cycle_read()});
            if (voice_assistant_reported)
                queries.insert(queries.end(), {read_query(table1_type, 0xf0, 4),
                                               InteractionParameters::voice_assistant_read()});
            if (guidance_type) {
                if (*guidance_type != 3)
                    queries.push_back(read_query(table2_type, 0x40, *guidance_type));
                queries.insert(queries.end(), {InteractionParameters::guidance_status(*guidance_type),
                                               read_query(table2_type, 0x46, *guidance_type)});
            }
            if (volume_reported)
                queries.push_back(InteractionParameters::volume_read());
            state.features[Feature::TouchAssignment].advertised = touch_type.has_value();
            state.features[Feature::AmbientCycle].advertised = cycle_reported;
            state.features[Feature::Notifications].advertised = guidance_type.has_value();
            state.features[Feature::PromptVolume].advertised = volume_reported;
            state.features[Feature::Multipoint].advertised =
                std::any_of(t1.functions.begin(), t1.functions.end(),
                            [](const auto &f) { return f.id >= 0xd1 && f.id <= 0xd4; });
            ++state.revision;
        }
        for (const auto &query_value : queries) {
            {
                std::lock_guard lock(gate);
                if (stopping || system_suspended || !radio_available || !desired_connected ||
                    (expected_intent && *expected_intent != connection_intent))
                    return failure(ResultCode::Canceled, "State reload canceled by connection environment.");
            }
            auto result = query(query_value);
            if (!result.ok())
                return result;
        }
        std::vector<Query> audio;
        {
            std::lock_guard lock(gate);
            alerts_registered = t1.contains(0x90) && state.model == "WF-1000XM5" &&
                                (multipoint_slot || profiles.contains(Feature::ConnectionPreference));
            if (alerts_registered)
                audio.push_back(AudioSourceParameters::enable_alerts());
            if (multipoint_slot)
                audio.insert(audio.end(), {AudioSourceParameters::general_availability(*multipoint_slot),
                                           AudioSourceParameters::general_read(*multipoint_slot)});
            if (source_type)
                audio.insert(audio.end(), {AudioSourceParameters::source_capability(*source_type),
                                           AudioSourceParameters::source_availability(*source_type),
                                           AudioSourceParameters::sources(*source_type)});
            if (fixation_reported)
                audio.push_back(InteractionParameters::fix_read());
            // Cycle may be a custom action embedded in the advertised touch layout.
            for (const auto &key : state.touch.keys)
                if (key.id == 0 || key.id == 4 || key.id == 5) {
                    const bool detailed =
                        std::any_of(key.presets.begin(), key.presets.end(), [](const auto &p) {
                            return std::any_of(
                                p.customizable.begin(), p.customizable.end(), [](const auto &a) {
                                    return a.action == 0 &&
                                           std::count_if(a.functions.begin(), a.functions.end(),
                                                         [](auto f) { return f >= 1 && f <= 4; }) >= 2;
                                });
                        });
                    if (detailed && !cycle_reported) {
                        cycle_key = key.id;
                        if (!cycle_reported && touch_type) {
                            cycle_details = cycle_reported = cycle_available = true;
                            state.features[Feature::AmbientCycle].advertised = true;
                            audio.push_back(InteractionParameters::touch_details_read(*touch_type));
                        }
                        break;
                    }
                }
        }
        for (const auto &request : audio) {
            {
                std::lock_guard lock(gate);
                if (stopping || system_suspended || !radio_available || !desired_connected ||
                    (expected_intent && *expected_intent != connection_intent))
                    return failure(ResultCode::Canceled, "State reload canceled by connection environment.");
            }
            auto result = query(request);
            if (!result.ok())
                return result;
        }
        auto codec = read_codec();
        if (!codec.ok())
            return codec;
        {
            std::lock_guard lock(gate);
            if (expected_intent && *expected_intent != connection_intent)
                return failure(ResultCode::Canceled, "Automatic recovery superseded.");
            state.connected = session.connected();
            if (!state.connected)
                return failure(ResultCode::NotConnected, "Control channel interrupted during bootstrap.");
            state.ready = true;
            state.status = "Ready; hardware validation pending";
            access();
            ++state.revision;
        }
        return {};
    }
    Result read_codec() {
        CodecQueryContext expected;
        {
            std::lock_guard lock(gate);
            if (!codec_reported || !state.sources.active_source.known())
                return {};
            invalidate_codec("Waiting for the current source's negotiated codec query.");
            expected = {*state.sources.active_source.value, state.link, codec_generation};
            codec_query_in_flight = expected;
        }
        const auto reply = session.exchange(ExtendedParameters::codec());
        {
            std::lock_guard lock(gate);
            auto buffered = std::move(buffered_codec_notification);
            buffered_codec_notification.reset();
            codec_query_in_flight.reset();
            if (!reply.result.ok())
                return reply.result;
            if (!reply.response)
                return failure(ResultCode::Protocol, "Device codec response was missing.");
            const auto parsed = parse(*reply.response, Family::V2);
            if (!parsed.valid())
                return failure(ResultCode::Protocol, "Malformed device codec response.");
            if (!session.connected() || expected.link != state.link || expected.link != session.link())
                return failure(ResultCode::Superseded, "Control channel changed during codec query.");
            apply(parsed.message, reply.receive_order, expected);
            // Only a successful query for this source generation releases the
            // fence. Earlier notifications, failed queries and changed sources
            // never get to turn an old-source codec back into Known.
            if (!codec_needs_query && buffered && current_codec_query(buffered->context) &&
                buffered->order > reply.receive_order)
                apply(buffered->message, buffered->order, buffered->context);
        }
        return {};
    }
};

DeviceController::DeviceController(SessionPolicy policy)
    : DeviceController(make_windows_rfcomm_transport(), policy) {}
DeviceController::DeviceController(std::unique_ptr<ByteTransport> transport, SessionPolicy policy)
    : impl_(std::make_unique<Impl>(std::move(transport), policy)) {}
DeviceController::~DeviceController() = default;
DiscoveryResult DeviceController::discover() const { return discover_paired_devices(); }
void DeviceController::set_trace(std::function<void(const ProtocolTrace &)> trace) {
    impl_->session.set_trace(std::move(trace));
}
DeviceSnapshot DeviceController::snapshot() const {
    std::lock_guard lock(impl_->gate);
    auto copy = impl_->state;
    if (!impl_->session.connected()) {
        copy.connected = copy.ready = false;
        copy.sound.writable = copy.equalizer.writable = copy.sources.multipoint_writable =
            copy.sources.switch_writable = false;
        copy.touch.writable = copy.touch.cycle_writable = copy.touch.guidance_writable =
            copy.touch.volume_writable = copy.touch.fixation_writable = copy.touch.assistant_writable = false;
        copy.safe_listening.writable = copy.safe_listening.available = false;
        stale(copy.safe_listening.enabled);
        stale(copy.safe_listening.preview);
        stale(copy.safe_listening.decibels);
        stale(copy.safe_listening.cause);
        for (auto &[feature, item] : copy.features) {
            (void)feature;
            item.writable = false;
            if (item.value.known())
                item.value.status = ValueStatus::Stale;
        }
        stale(copy.battery.single);
        stale(copy.battery.left);
        stale(copy.battery.right);
        stale(copy.battery.case_level);
        stale(copy.battery.single_charging);
        stale(copy.battery.left_charging);
        stale(copy.battery.right_charging);
        stale(copy.battery.case_charging);
        stale(copy.sound.mode);
        stale(copy.sound.ambient_level);
        stale(copy.sound.voice_focus);
        stale(copy.equalizer.preset);
        stale(copy.equalizer.bands);
        stale(copy.equalizer.clear_bass);
        stale(copy.sources.active_source);
        stale(copy.sources.multipoint);
        stale(copy.codec);
        stale(copy.touch.assignments);
        stale(copy.touch.details);
        stale(copy.touch.cycle);
        stale(copy.touch.guidance);
        stale(copy.touch.prompt_volume);
        stale(copy.touch.voice_assistant);
        stale(copy.touch.playback_fixed);
        copy.sound.available = copy.equalizer.available = copy.sources.available = false;
        for (auto &[feature, capability] : copy.capabilities.features) {
            (void)feature;
            capability.access.write = Support::Unknown;
        }
    }
    return copy;
}
Result DeviceController::connect(const DeviceDescriptor &device) {
    return impl_->command([&] {
        {
            std::lock_guard lock(impl_->gate);
            if (impl_->system_suspended || !impl_->radio_available)
                return failure(ResultCode::Unavailable,
                               "Restore Windows Bluetooth and resume the system before connecting.");
            impl_->desired_connected = true;
            ++impl_->connection_intent;
            impl_->recovery_budget = 1;
        }
        impl_->session.disconnect();
        {
            std::lock_guard lock(impl_->gate);
            impl_->reset_for_link(device);
        }
        auto result = impl_->session.connect(device);
        if (!result.ok())
            return result;
        {
            std::lock_guard lock(impl_->gate);
            impl_->state.link = impl_->session.link();
            impl_->state.connected = true;
            impl_->state.status = "Negotiating MDR protocol";
        }
        result = impl_->bootstrap();
        if (!result.ok()) {
            impl_->session.disconnect();
            std::lock_guard lock(impl_->gate);
            impl_->desired_connected = false;
        }
        return result;
    });
}
void DeviceController::disconnect() noexcept {
    {
        std::lock_guard lock(impl_->gate);
        impl_->desired_connected = false;
        ++impl_->connection_intent;
        impl_->recovery_budget = 0;
    }
    impl_->session.disconnect();
    std::lock_guard lock(impl_->gate);
    impl_->state.connected = impl_->state.ready = false;
    impl_->state.status = "Disconnected";
    impl_->state.touch.linked_backup_assignments.reset();
    impl_->state.touch.linked_backup_assistant.reset();
    impl_->access();
    ++impl_->state.revision;
    impl_->alerts_changed.notify_all();
    impl_->safe_listening_changed.notify_all();
}
void DeviceController::suspend() noexcept {
    {
        std::lock_guard lock(impl_->gate);
        impl_->system_suspended = true;
        ++impl_->connection_intent;
    }
    impl_->session.abort();
    {
        std::lock_guard lock(impl_->gate);
        impl_->state.connected = impl_->state.ready = false;
        impl_->state.status = "System suspended; waiting for resume";
        impl_->state.touch.linked_backup_assignments.reset();
        impl_->state.touch.linked_backup_assistant.reset();
        impl_->access();
        ++impl_->state.revision;
    }
    impl_->alerts_changed.notify_all();
    impl_->safe_listening_changed.notify_all();
}
Result DeviceController::resume() {
    {
        std::lock_guard lock(impl_->gate);
        const bool changed = impl_->system_suspended;
        if (changed)
            ++impl_->connection_intent;
        impl_->system_suspended = false;
        if (changed && impl_->desired_connected)
            impl_->recovery_budget = 1;
        if (!impl_->radio_available)
            return failure(ResultCode::Unavailable, "System resumed; waiting for Bluetooth availability.");
        if (!impl_->desired_connected)
            return {};
        impl_->state.status = "System resumed; bounded read-only recovery pending";
        ++impl_->state.revision;
    }
    impl_->recovery_changed.notify_all();
    return {};
}
void DeviceController::notify_bluetooth_available(bool available) noexcept {
    {
        std::lock_guard lock(impl_->gate);
        const bool changed = impl_->radio_available != available;
        impl_->radio_available = available;
        if (!changed)
            return;
        ++impl_->connection_intent;
        if (available && impl_->desired_connected)
            impl_->recovery_budget = 1;
    }
    if (!available) {
        impl_->session.abort();
        std::lock_guard lock(impl_->gate);
        impl_->state.connected = impl_->state.ready = false;
        impl_->state.status = "Bluetooth unavailable; waiting for the adapter";
        impl_->access();
        ++impl_->state.revision;
    }
    impl_->recovery_changed.notify_all();
    impl_->alerts_changed.notify_all();
}
Result DeviceController::refresh() {
    return impl_->command([&] {
        if (!impl_->session.connected())
            return failure(ResultCode::NotConnected, "Connect the MDR control channel first.");
        return impl_->bootstrap();
    });
}
Result DeviceController::set_safe_listening(bool enabled) {
    return impl_->command([&] {
        Byte type;
        std::uint64_t baseline{}, link{};
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            auto &sl = impl_->state.safe_listening;
            if (!sl.writable || !sl.type)
                return failure(ResultCode::Unavailable, "安全聆听能力、可用性或更新间隔尚未确认。");
            if (sl.enabled.known() && *sl.enabled.value == enabled && sl.preview.known() && !*sl.preview.value)
                return Result{};
            type = *sl.type;
            link = impl_->state.link;
            baseline = sl.enabled.receive_order;
            unknown(sl.enabled, "Waiting for Safe Listening mode confirmation.");
            unknown(sl.decibels, "Waiting for Safe Listening mode confirmation.");
            unknown(sl.cause, "Waiting for Safe Listening mode confirmation.");
            sl.updated_at.reset();
        }
        const auto sent = impl_->query(SafeListeningParameters::set(type, enabled));
        if (!sent.ok())
            return sent;
        std::unique_lock lock(impl_->gate);
        const auto confirmed = impl_->safe_listening_changed.wait_for(
            lock, impl_->mode_confirmation_timeout, [&] {
                return impl_->stopping || !impl_->session.connected() || impl_->state.link != link ||
                       impl_->state.safe_listening.enabled.receive_order > baseline;
            });
        auto &sl = impl_->state.safe_listening;
        if (impl_->stopping || !impl_->session.connected() || impl_->state.link != link)
            return failure(ResultCode::NotConnected, "确认安全聆听时连接已断开。");
        if (!confirmed || !sl.enabled.known() || !sl.preview.known()) {
            unknown(sl.enabled, "Safe Listening mode was not confirmed.");
            sl.reason = "耳机已接收指令，但未确认安全聆听状态；请重试。";
            return failure(ResultCode::Timeout, sl.reason);
        }
        if (*sl.enabled.value != enabled || *sl.preview.value)
            return failure(ResultCode::DeviceRejected, "耳机返回的安全聆听状态与请求不一致。");
        sl.reason.clear();
        impl_->next_pressure_read = {};
        return Result{};
    });
}
Result DeviceController::poll_playback_fixed() {
    std::unique_lock operation(impl_->operations, std::try_to_lock);
    if (!operation.owns_lock())
        return failure(ResultCode::Busy, "Another device operation is running.");
    std::pair<std::uint64_t, std::uint64_t> expected;
    {
        std::lock_guard lock(impl_->gate);
        if (!impl_->session.connected() || !impl_->state.ready || !impl_->fixation_reported ||
            !impl_->state.sources.available || !impl_->state.sources.multipoint.known() ||
            !*impl_->state.sources.multipoint.value || !impl_->state.sources.active_source.known())
            return failure(ResultCode::Unavailable, "Playback fixation readback is not available.");
        if (impl_->state.touch.playback_fixed.known()) return {};
        expected = {impl_->state.link, impl_->fixation_generation};
        impl_->fixation_query = expected;
    }
    auto result = impl_->query(InteractionParameters::fix_read());
    {
        std::lock_guard lock(impl_->gate);
        if (impl_->fixation_query == expected) impl_->fixation_query.reset();
        if (impl_->state.link != expected.first)
            return failure(ResultCode::Canceled, "Fixation readback belongs to an old connection.");
        if (impl_->fixation_generation != expected.second) {
            unknown(impl_->state.touch.playback_fixed, "Source changed; refresh fixation state.");
            impl_->access();
            ++impl_->state.revision;
            return failure(ResultCode::Canceled, "Active source changed during fixation readback.");
        }
    }
    return result;
}
Result DeviceController::poll_codec() {
    std::unique_lock operation(impl_->operations, std::try_to_lock);
    if (!operation.owns_lock())
        return failure(ResultCode::Busy, "Another device operation is running.");
    {
        std::lock_guard lock(impl_->gate);
        if (!impl_->session.connected() || !impl_->state.ready || !impl_->codec_reported ||
            !impl_->state.sources.active_source.known())
            return failure(ResultCode::Unavailable, "Codec readback is not available.");
        if (!impl_->codec_needs_query) return {};
    }
    auto result = impl_->read_codec();
    std::lock_guard lock(impl_->gate);
    if (result.ok() && impl_->codec_needs_query)
        return failure(ResultCode::Superseded, "Source changed during codec readback.");
    return result;
}
Result DeviceController::poll_sound_pressure() {
    std::unique_lock operation(impl_->operations, std::try_to_lock);
    if (!operation.owns_lock())
        return failure(ResultCode::Busy, "Another device operation is running.");
    Byte type;
    std::uint64_t link{};
    {
        std::lock_guard lock(impl_->gate);
        const auto &sl = impl_->state.safe_listening;
        if (!impl_->session.connected() || !impl_->state.ready || !sl.available || !sl.type ||
            !sl.enabled.known() || !*sl.enabled.value || !sl.minimum_interval_seconds ||
            *sl.minimum_interval_seconds == 0)
            return failure(ResultCode::Unavailable, "Safe Listening is not enabled or available.");
        const auto now = std::chrono::steady_clock::now();
        if (now < impl_->next_pressure_read)
            return {};
        type = *sl.type;
        link = impl_->state.link;
        impl_->next_pressure_read = now + std::chrono::seconds(*sl.minimum_interval_seconds);
    }
    auto result = impl_->query(SafeListeningParameters::current(type));
    if (!result.ok()) {
        std::lock_guard lock(impl_->gate);
        if (impl_->state.link == link) {
            auto &sl = impl_->state.safe_listening;
            stale(sl.decibels);
            stale(sl.cause);
            sl.reason = "音压读取失败，等待下一次更新或重新连接。";
            ++impl_->state.revision;
        }
    }
    return result;
}
Result DeviceController::set_sound(std::optional<SoundMode> mode, std::optional<int> level,
                                   std::optional<bool> voice) {
    return impl_->command([&] {
        Query write;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.sound.writable || !impl_->sound)
                return failure(ResultCode::Unavailable,
                               "Sound layout, model and availability must be confirmed before writing.");
            if (!mode && !level && !voice)
                return failure(ResultCode::InvalidValue, "No sound change was requested.");
            if (mode && *mode != SoundMode::NoiseCanceling && *mode != SoundMode::Ambient &&
                *mode != SoundMode::Off)
                return failure(ResultCode::InvalidValue, "Invalid sound mode.");
            if (level && (*level < 0 || *level > 20))
                return failure(ResultCode::InvalidValue,
                               "Ambient level must be in the verified 0 to 20 layout.");
            if (voice && (impl_->sound->sound_mode() != SoundMode::Ambient || impl_->sound->level > 20))
                return failure(ResultCode::InvalidValue,
                               "Read and enable ambient mode before changing voice focus.");
            const auto ambient_mode =
                static_cast<Byte>(voice.value_or(impl_->sound->ambient_mode == 1) ? 1 : 0);
            const auto range = impl_->state.sound.ranges.find(ambient_mode);
            if ((level || voice) && (range == impl_->state.sound.ranges.end() ||
                                     !range->second.contains(level.value_or(impl_->sound->level))))
                return failure(ResultCode::InvalidValue,
                               "Ambient level/voice focus is outside the reported range.");
            write = impl_->sound->set(mode, level, voice);
        }
        return impl_->confirm(write, SoundParameters::read(), [&] {
            return impl_->sound && impl_->sound->settled && (!mode || impl_->sound->sound_mode() == *mode) &&
                   (!level || impl_->sound->level == *level) &&
                   (!voice || (impl_->sound->ambient_mode == 1) == *voice);
        });
    });
}
Result DeviceController::set_equalizer(std::optional<Byte> preset, std::optional<int> band,
                                       std::optional<int> gain, std::optional<int> bass) {
    return impl_->command([&] {
        Byte target;
        Bytes steps;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.equalizer.writable || !impl_->equalizer || !impl_->equalizer_cap)
                return failure(ResultCode::Unavailable, impl_->state.equalizer.reason);
            if ((!preset && !band && !bass) || (band.has_value() != gain.has_value()) ||
                (band && (*band < 0 || *band > 4)) || (gain && (*gain < -10 || *gain > 10)) ||
                (bass && (*bass < -10 || *bass > 10)) || (preset && (band || bass)))
                return failure(ResultCode::InvalidValue, "Invalid EQ change.");
            if (preset) {
                target = *preset;
            } else {
                if (!impl_->equalizer->five_bands() || impl_->equalizer->preset_id == 0)
                    return failure(ResultCode::Unavailable,
                                   "Enable an EQ preset and read all six values before editing.");
                // A1/A2 are user slots. Editing their curve updates the same slot;
                // editing a built-in curve creates the manual (A0) curve instead.
                const auto current = impl_->equalizer->preset_id;
                target = current == 0xa1 || current == 0xa2 ? current : Byte{0xa0};
                steps = impl_->equalizer->steps;
                if (band)
                    steps[static_cast<std::size_t>(*band) + 1] = static_cast<Byte>(*gain + 10);
                if (bass)
                    steps[0] = static_cast<Byte>(*bass + 10);
            }
            if (std::none_of(impl_->equalizer_cap->presets.begin(), impl_->equalizer_cap->presets.end(),
                             [&](const auto &p) { return p.id == target; }))
                return failure(ResultCode::InvalidValue, "Device did not advertise this EQ preset.");
        }
        return impl_->confirm(EqualizerParameters::set(target, steps), EqualizerParameters::read(), [&] {
            return impl_->equalizer && impl_->equalizer->preset_id == target &&
                   (steps.empty() || impl_->equalizer->steps == steps);
        });
    });
}
Result DeviceController::switch_source(const std::string &address) {
    return impl_->command([&] {
        Byte subtype;
        std::uint64_t baseline;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.sources.switch_writable || !impl_->source_type)
                return failure(ResultCode::Unavailable, "Source switch dependencies are not confirmed.");
            if (std::none_of(impl_->state.sources.entries.begin(), impl_->state.sources.entries.end(),
                             [&](const auto &e) { return e.id == address && e.connection_slot != 0; }))
                return failure(ResultCode::InvalidValue,
                               "Target is not in the current connected source list.");
            subtype = *impl_->source_type;
            baseline = impl_->switch_result_order;
        }
        auto result = impl_->confirm(
            AudioSourceParameters::switch_source(address), AudioSourceParameters::sources(subtype), [&] {
                return impl_->state.sources.active_source.known() &&
                       *impl_->state.sources.active_source.value == address &&
                       !(impl_->switch_result_order > baseline && impl_->switch_result &&
                         impl_->switch_result->target == address && impl_->switch_result->result != 0);
            });
        if (!result.ok())
            return result;
        if (impl_->fixation_reported) {
            result = impl_->query(InteractionParameters::fix_read());
            if (!result.ok())
                return result;
        }
        return impl_->read_codec();
    });
}
Result DeviceController::set(Feature feature, int value) {
    if (feature == Feature::SoundMode) {
        if (value < 0 || value > 2)
            return failure(ResultCode::InvalidValue, "Invalid sound mode.");
        return set_sound(static_cast<SoundMode>(value));
    }
    if (feature == Feature::AmbientLevel)
        return set_sound({}, value);
    if (feature == Feature::VoiceFocus) {
        if (value != 0 && value != 1)
            return failure(ResultCode::InvalidValue, "Invalid voice focus.");
        return set_sound({}, {}, value == 1);
    }
    if (feature == Feature::Equalizer) {
        if (value < 0 || value > 255)
            return failure(ResultCode::InvalidValue, "Invalid EQ preset.");
        return set_equalizer(static_cast<Byte>(value));
    }
    if (feature == Feature::ClearBass)
        return set_equalizer({}, {}, {}, value);
    return impl_->command([&] {
        if (feature == Feature::Multipoint) {
            Byte slot;
            std::optional<Byte> subtype;
            bool alerts;
            std::uint64_t baseline;
            {
                std::lock_guard lock(impl_->gate);
                impl_->access();
                if (!impl_->state.sources.multipoint_writable || !impl_->multipoint_slot)
                    return failure(ResultCode::Unavailable,
                                   "Multipoint setting was not identified and read.");
                if (value != 0 && value != 1)
                    return failure(ResultCode::InvalidValue, "Invalid multipoint value.");
                slot = *impl_->multipoint_slot;
                subtype = impl_->source_type;
                alerts = impl_->alerts_registered;
                baseline = impl_->alert_order;
            }
            auto result = impl_->query(AudioSourceParameters::set_multipoint(slot, value == 1));
            if (!result.ok())
                return result;
            if (alerts)
                if (const auto alert = impl_->wait_alert(baseline)) {
                    if (alert->action != 1 || (alert->message != 6 && alert->message != 7))
                        return failure(ResultCode::DeviceRejected,
                                       "Unrecognized multipoint alert; no automatic confirmation was sent.");
                    result = impl_->query(AudioSourceParameters::confirm_multipoint_alert(alert->message));
                    if (!result.ok())
                        return result;
                    std::this_thread::sleep_for(Milliseconds{300});
                }
            result = impl_->query(AudioSourceParameters::general_read(slot));
            if (!result.ok())
                return result;
            if (subtype) {
                result = impl_->query(AudioSourceParameters::sources(*subtype));
                if (!result.ok())
                    return result;
            }
            result = impl_->read_codec();
            if (!result.ok())
                return result;
            std::lock_guard lock(impl_->gate);
            return impl_->state.sources.multipoint.known() &&
                           *impl_->state.sources.multipoint.value == (value == 1)
                       ? Result{}
                       : failure(ResultCode::DeviceRejected,
                                 "Multipoint readback did not confirm the target.");
        }
        ExtendedProfile profile;
        Byte last = 0;
        bool extended = false, alerts = false;
        std::uint64_t baseline = 0;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (impl_->profiles.contains(feature)) {
                const auto &item = impl_->state.features[feature];
                if (!item.writable ||
                    std::find(item.choices.begin(), item.choices.end(), value) == item.choices.end())
                    return failure(ResultCode::Unavailable,
                                   "Model, candidate values and setting availability must be confirmed.");
                profile = impl_->profiles[feature];
                last = impl_->last_selected[feature];
                extended = true;
                alerts = feature == Feature::ConnectionPreference && impl_->alerts_registered;
                baseline = impl_->alert_order;
                if (feature == Feature::ConnectionPreference)
                    impl_->invalidate_codec("Connection preference changed; refresh codec.");
            }
        }
        if (extended) {
            auto result = impl_->query(ExtendedParameters::write(profile, static_cast<Byte>(value), last));
            if (!result.ok())
                return result;
            if (feature == Feature::PowerOff)
                return Result{ResultCode::Delivered,
                              "Power-off command delivered; ACK/disconnect cannot prove the headphones are "
                              "powered off. Verify on hardware."};
            if (alerts)
                if (const auto alert = impl_->wait_alert(baseline)) {
                    if (alert->message != 0 || alert->action != 1)
                        return failure(
                            ResultCode::DeviceRejected,
                            "Unrecognized connection preference alert; no automatic confirmation was sent.");
                    result = impl_->query(ExtendedParameters::confirm_connection_change());
                    if (!result.ok())
                        return result;
                }
            result = impl_->query(ExtendedParameters::read(profile));
            if (!result.ok())
                return result;
            {
                std::lock_guard lock(impl_->gate);
                const auto &item = impl_->state.features[feature];
                if (!item.value.known() || *item.value.value != value)
                    return failure(ResultCode::DeviceRejected,
                                   "Extended setting readback did not confirm the target.");
                if (profile.function == 0xe7 && impl_->last_selected[feature] != 0)
                    return Result{ResultCode::Delivered, "Preference read back, but the connection stream is "
                                                         "not stable. Refresh before another write."};
            }
            return feature == Feature::ConnectionPreference ? impl_->read_codec() : Result{};
        }
        Query write, read;
        std::optional<std::string> fixed_target;
        std::uint64_t fixation_baseline = 0;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.features[feature].writable)
                return failure(ResultCode::Unavailable,
                               "This setting's model, capability or dependencies are not confirmed.");
            if (feature == Feature::AmbientCycle) {
                if (value < 1 || value > 4)
                    return failure(ResultCode::InvalidValue,
                                   "An ambient cycle must contain at least two modes.");
                if (impl_->cycle_details) {
                    const auto preset = impl_->active_preset(*impl_->cycle_key);
                    if (!preset || !impl_->state.touch.details.known() ||
                        std::none_of(preset->customizable.begin(), preset->customizable.end(),
                                     [&](const auto &a) {
                                         return a.action == 0 &&
                                                std::find(a.functions.begin(), a.functions.end(),
                                                          static_cast<Byte>(value)) != a.functions.end();
                                     }))
                        return failure(ResultCode::InvalidValue,
                                       "Device did not advertise this ambient cycle action.");
                    auto details = *impl_->state.touch.details.value;
                    bool changed = false;
                    for (auto &state : details)
                        if (state.preset == preset->id)
                            for (auto &action : state.actions)
                                if (action.action == 0) {
                                    action.function = static_cast<Byte>(value);
                                    changed = true;
                                }
                    if (!changed)
                        return failure(ResultCode::Unavailable, "Current cycle action was not read.");
                    write = InteractionParameters::touch_details_write(*impl_->touch_type, details);
                    read = InteractionParameters::touch_details_read(*impl_->touch_type);
                } else {
                    write = InteractionParameters::cycle_write(static_cast<Byte>(value));
                    read = InteractionParameters::cycle_read();
                }
            } else if (feature == Feature::Notifications) {
                if (value != 0 && value != 1)
                    return failure(ResultCode::InvalidValue, "Invalid guidance switch.");
                write = InteractionParameters::guidance_write(*impl_->guidance_type, static_cast<Byte>(value),
                                                              impl_->guidance_language);
                read = read_query(table2_type, 0x46, *impl_->guidance_type);
            } else if (feature == Feature::PromptVolume) {
                if (value < -2 || value > 2)
                    return failure(ResultCode::InvalidValue, "Prompt volume is -2 to +2.");
                write = InteractionParameters::volume_write(value);
                read = InteractionParameters::volume_read();
            } else if (feature == Feature::FixedPlayback) {
                if (value != 0 && value != 1)
                    return failure(ResultCode::InvalidValue, "Invalid playback fixation.");
                fixed_target = impl_->state.sources.active_source.value;
                fixation_baseline = impl_->fixation_result_order;
                write = InteractionParameters::fix_write(value == 1);
                read = InteractionParameters::fix_read();
            } else
                return failure(ResultCode::Unsupported, "Use the setting-specific API for this feature.");
        }
        if (feature == Feature::FixedPlayback && impl_->source_type) {
            auto result = impl_->query(AudioSourceParameters::sources(*impl_->source_type));
            if (!result.ok())
                return result;
            std::lock_guard lock(impl_->gate);
            if (!fixed_target || !impl_->state.sources.active_source.known() ||
                impl_->state.sources.active_source.value != fixed_target)
                return failure(ResultCode::DeviceRejected,
                               "Active source changed before fixation; no write was sent.");
        }
        return impl_->confirm(write, read, [&] {
            const auto &touch = impl_->state.touch;
            if (feature == Feature::AmbientCycle)
                return touch.cycle.known() && *touch.cycle.value == value;
            if (feature == Feature::Notifications)
                return touch.guidance.known() && *touch.guidance.value == value;
            if (feature == Feature::PromptVolume)
                return touch.prompt_volume.known() && *touch.prompt_volume.value == value;
            return touch.playback_fixed.known() && *touch.playback_fixed.value == (value == 1) &&
                   (value == 0 || impl_->state.sources.active_source.value == fixed_target) &&
                   !(impl_->fixation_result_order > fixation_baseline && impl_->fixation_result != 0);
        });
    });
}
Result DeviceController::set_touch_details(const std::vector<TouchPresetState> &details) {
    return impl_->command([&] {
        Byte type;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.touch.writable || !impl_->touch_type || !impl_->state.touch.details.known())
                return failure(ResultCode::Unavailable,
                               "Current touch details and availability must be read first.");
            const auto &original = *impl_->state.touch.details.value;
            if (details.size() != original.size())
                return failure(ResultCode::InvalidValue,
                               "Touch detail updates must preserve the full captured preset list.");
            std::set<Byte> preset_ids;
            for (const auto &selected : details) {
                if (!preset_ids.insert(selected.preset).second)
                    return failure(ResultCode::InvalidValue, "Duplicate touch preset.");
                const auto previous = std::find_if(original.begin(), original.end(), [&](const auto &value) {
                    return value.preset == selected.preset;
                });
                if (previous == original.end() || selected.actions.size() != previous->actions.size())
                    return failure(ResultCode::InvalidValue,
                                   "Touch detail updates must preserve captured presets and actions.");
                std::set<Byte> action_ids;
                for (const auto &action : selected.actions)
                    if (!action_ids.insert(action.action).second)
                        return failure(ResultCode::InvalidValue, "Duplicate touch action.");
                bool allowed = false;
                for (std::size_t key_index = 0; key_index < impl_->state.touch.keys.size(); ++key_index) {
                    const auto &key = impl_->state.touch.keys[key_index];
                    for (const auto &preset : key.presets)
                        if (preset.id == selected.preset) {
                            allowed |= std::all_of(
                                selected.actions.begin(), selected.actions.end(), [&](const auto &action) {
                                    const auto captured = std::find_if(
                                        previous->actions.begin(), previous->actions.end(),
                                        [&](const auto &a) { return a.action == action.action; });
                                    if (captured == previous->actions.end())
                                        return false;
                                    if (*captured == action)
                                        return true;
                                    const bool current_available_key =
                                        key_index < impl_->key_availability.size() &&
                                        impl_->key_availability[key_index] == 0 &&
                                        impl_->state.touch.assignments.known() &&
                                        key_index < impl_->state.touch.assignments.value->size() &&
                                        (*impl_->state.touch.assignments.value)[key_index] == preset.id;
                                    return current_available_key &&
                                           std::any_of(preset.customizable.begin(), preset.customizable.end(),
                                                       [&](const auto &a) {
                                                           return a.action == action.action &&
                                                                  std::find(
                                                                      a.functions.begin(), a.functions.end(),
                                                                      action.function) != a.functions.end();
                                                       });
                                });
                        }
                }
                if (!allowed)
                    return failure(ResultCode::InvalidValue,
                                   "Touch details contain an unadvertised action/function.");
            }
            type = *impl_->touch_type;
        }
        const auto write = InteractionParameters::touch_details_write(type, details);
        return impl_->confirm(write, InteractionParameters::touch_details_read(type), [&] {
            return impl_->state.touch.details.known() &&
                   InteractionParameters::touch_details_write(type, *impl_->state.touch.details.value)
                           .payload == write.payload;
        });
    });
}
Result DeviceController::set_voice_assistant(Byte value) {
    return impl_->command([&] {
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.touch.assistant_writable ||
                std::find(impl_->state.touch.voice_assistant_candidates.begin(),
                          impl_->state.touch.voice_assistant_candidates.end(),
                          value) == impl_->state.touch.voice_assistant_candidates.end())
                return failure(ResultCode::Unavailable,
                               "Voice assistant candidate, model or current state is not confirmed.");
        }
        return impl_->confirm(InteractionParameters::voice_assistant_write(value),
                              InteractionParameters::voice_assistant_read(), [&] {
                                  return impl_->state.touch.voice_assistant.known() &&
                                         *impl_->state.touch.voice_assistant.value == value;
                              });
    });
}
Result DeviceController::set_touch(Byte key, Byte preset, bool confirm_linked_change) {
    return impl_->command([&] {
        if (impl_->voice_assistant_reported) {
            auto result = impl_->query(read_query(table1_type, 0xf0, 4));
            if (!result.ok())
                return result;
            result = impl_->query(InteractionParameters::voice_assistant_read());
            if (!result.ok())
                return result;
        }
        Bytes expected;
        Byte type;
        std::uint64_t baseline;
        {
            std::lock_guard lock(impl_->gate);
            impl_->access();
            if (!impl_->state.touch.writable || !impl_->touch_type || !impl_->can_assign())
                return failure(ResultCode::Unavailable,
                               "Touch capability, actual assignments and availability must be read first.");
            const auto &touch = impl_->state.touch;
            const auto selected = std::find_if(touch.keys.begin(), touch.keys.end(),
                                               [&](const auto &k) { return k.id == key; });
            if (selected == touch.keys.end() ||
                impl_->key_availability[static_cast<std::size_t>(selected - touch.keys.begin())] != 0 ||
                !InteractionParameters::preset_allowed(preset, impl_->limitation) ||
                std::none_of(selected->presets.begin(), selected->presets.end(),
                             [&](const auto &p) { return p.id == preset; }))
                return failure(ResultCode::InvalidValue,
                               "Touch key/preset is unavailable, restricted or unadvertised.");
            if (key == 1 && preset == 0xff && impl_->voice_assistant_reported) {
                if (!touch.voice_assistant.known() || impl_->assistant_key_type != 3 ||
                    std::find(touch.voice_assistant_candidates.begin(),
                              touch.voice_assistant_candidates.end(),
                              0xff) == touch.voice_assistant_candidates.end())
                    return failure(
                        ResultCode::Unavailable,
                        "Linked voice assistant state was not captured; right-ear disable is blocked.");
                if (*touch.voice_assistant.value != 0xff && !confirm_linked_change)
                    return failure(
                        ResultCode::ConfirmationRequired,
                        "Disabling this right-ear assignment can disable the linked voice assistant. Confirm "
                        "to capture and restore both settings in this connection.");
                if (!impl_->state.touch.linked_backup_assignments) {
                    impl_->state.touch.linked_backup_assignments = touch.assignments.value;
                    impl_->state.touch.linked_backup_assistant = *touch.voice_assistant.value;
                }
            }
            expected = *touch.assignments.value;
            expected[static_cast<std::size_t>(selected - touch.keys.begin())] = preset;
            type = *impl_->touch_type;
            baseline = impl_->alert_order;
        }
        auto result = impl_->query(InteractionParameters::touch_write(type, expected));
        if (!result.ok())
            return result;
        if (const auto alert = impl_->wait_alert(baseline)) {
            if (alert->message != 0x0f || alert->action != 1 || key != 1)
                return failure(ResultCode::DeviceRejected,
                               "Unrecognized touch alert; no automatic confirmation was sent.");
            result =
                impl_->query(InteractionParameters::reply_touch_voice_assistant_alert(confirm_linked_change));
            if (!result.ok())
                return result;
            if (!confirm_linked_change)
                return failure(ResultCode::ConfirmationRequired,
                               "Headphones requested permission to disable the linked assistant. The alert "
                               "was declined; confirm and retry to capture both settings.");
            std::this_thread::sleep_for(Milliseconds{300});
        }
        result = impl_->query(InteractionParameters::touch_read(type));
        if (!result.ok())
            return result;
        if (impl_->voice_assistant_reported) {
            result = impl_->query(InteractionParameters::voice_assistant_read());
            if (!result.ok())
                return result;
        }
        std::optional<int> restore;
        {
            std::lock_guard lock(impl_->gate);
            const auto &touch = impl_->state.touch;
            if (!touch.assignments.known() || *touch.assignments.value != expected)
                return failure(ResultCode::DeviceRejected, "Touch readback did not confirm the assignments.");
            if (key == 1 && touch.linked_backup_assignments &&
                touch.linked_backup_assignments->size() == expected.size()) {
                const auto selected = std::find_if(touch.keys.begin(), touch.keys.end(),
                                                   [](const auto &k) { return k.id == 1; });
                if (selected != touch.keys.end()) {
                    const auto index = static_cast<std::size_t>(selected - touch.keys.begin());
                    if ((*touch.linked_backup_assignments)[index] == expected[index])
                        restore = touch.linked_backup_assistant;
                }
            }
        }
        if (restore) {
            bool must_restore;
            std::uint64_t alert_baseline;
            {
                std::lock_guard lock(impl_->gate);
                const auto &touch = impl_->state.touch;
                must_restore = !touch.voice_assistant.known() || *touch.voice_assistant.value != *restore;
                alert_baseline = impl_->alert_order;
                if (must_restore &&
                    (!touch.voice_assistant.known() || *touch.voice_assistant.value != 0xff ||
                     std::find(touch.voice_assistant_candidates.begin(),
                               touch.voice_assistant_candidates.end(),
                               static_cast<Byte>(*restore)) == touch.voice_assistant_candidates.end()))
                    return failure(ResultCode::DeviceRejected,
                                   "Touch assignment restored, but assistant changed independently; saved "
                                   "settings were retained without overwriting it.");
            }
            if (must_restore) {
                result =
                    impl_->query(InteractionParameters::voice_assistant_write(static_cast<Byte>(*restore)));
                if (!result.ok())
                    return result;
                if (const auto alert = impl_->wait_alert(alert_baseline)) {
                    if (alert->message != 0x13 || alert->action != 1)
                        return failure(ResultCode::DeviceRejected,
                                       "Unrecognized restore alert; linked backup retained.");
                    result = impl_->query(InteractionParameters::reply_restore_voice_assistant_alert());
                    if (!result.ok())
                        return result;
                    std::this_thread::sleep_for(Milliseconds{300});
                }
                result = impl_->query(InteractionParameters::voice_assistant_read());
                if (!result.ok())
                    return result;
                result = impl_->query(InteractionParameters::touch_read(type));
                if (!result.ok())
                    return result;
            }
            std::lock_guard lock(impl_->gate);
            if (!impl_->state.touch.voice_assistant.known() ||
                *impl_->state.touch.voice_assistant.value != *restore ||
                !impl_->state.touch.assignments.known() || *impl_->state.touch.assignments.value != expected)
                return failure(ResultCode::DeviceRejected,
                               "Linked restore did not confirm both values; backup retained.");
            impl_->state.touch.linked_backup_assignments.reset();
            impl_->state.touch.linked_backup_assistant.reset();
        }
        if (impl_->cycle_details) {
            result = impl_->query(InteractionParameters::touch_details_read(type));
            if (!result.ok())
                return result;
        }
        return Result{};
    });
}
Result DeviceController::restore_touch_linked_backup() {
    Byte preset;
    {
        std::lock_guard lock(impl_->gate);
        const auto &touch = impl_->state.touch;
        if (!touch.linked_backup_assignments || !touch.linked_backup_assistant)
            return failure(ResultCode::Unavailable, "No linked settings backup exists in this connection.");
        const auto right =
            std::find_if(touch.keys.begin(), touch.keys.end(), [](const auto &key) { return key.id == 1; });
        if (right == touch.keys.end() ||
            static_cast<std::size_t>(right - touch.keys.begin()) >= touch.linked_backup_assignments->size())
            return failure(ResultCode::Unavailable,
                           "Right-ear assignment no longer matches the captured layout.");
        preset = (*touch.linked_backup_assignments)[static_cast<std::size_t>(right - touch.keys.begin())];
    }
    return set_touch(1, preset, false);
}
} // namespace sonyconnect
