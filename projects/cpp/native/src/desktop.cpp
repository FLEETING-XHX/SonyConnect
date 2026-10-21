#include "sonyconnect/desktop.hpp"
#include "sonyconnect/desktop_async.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Media.Control.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace sonyconnect::desktop {
namespace {
using Microsoft::WRL::ComPtr;
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
std::wstring wide(const std::string &text) { return winrt::to_hstring(text).c_str(); }
void checked(HRESULT result) { winrt::check_hresult(result); }
std::string read_file(const std::filesystem::path &path, std::size_t limit) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read file");
    std::string result;
    std::array<char, 8192> buffer{};
    while (file) {
        file.read(buffer.data(), buffer.size());
        auto count = static_cast<std::size_t>(file.gcount());
        if (result.size() + count > limit)
            throw std::runtime_error("File exceeds limit");
        result.append(buffer.data(), count);
    }
    if (file.bad())
        throw std::runtime_error("File read failed");
    return result;
}
void atomic_write(const std::filesystem::path &path, const std::string &text, bool backup) {
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    auto temp = path;
    temp += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    try {
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            output.write(text.data(), text.size());
            output.flush();
            if (!output)
                throw std::runtime_error("File write failed");
        }
        if (std::filesystem::exists(path)) {
            auto bak = path;
            bak += L".bak";
            if (!ReplaceFileW(path.c_str(), temp.c_str(), backup ? bak.c_str() : nullptr, 0, nullptr,
                              nullptr))
                throw std::system_error(GetLastError(), std::system_category(), "ReplaceFile");
        } else if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "MoveFile");
    } catch (...) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        throw;
    }
}
} // namespace

bool MediaState::allows(MediaAction action) const {
    switch (action) {
    case MediaAction::play:
        return can_play;
    case MediaAction::pause:
        return can_pause;
    case MediaAction::previous:
        return can_previous;
    case MediaAction::next:
        return can_next;
    }
    return false;
}
std::wstring bluetooth_aep_id(std::uint64_t address, const std::atomic_bool *closing) {
    if (!address || address > 0xffffffffffffULL)
        return {};
    try {
        auto device = detail::await_async([&] {
            return winrt::Windows::Devices::Bluetooth::BluetoothDevice::FromBluetoothAddressAsync(address);
        }, closing);
        if (!device)
            return {};
        std::wstring id = device.DeviceInformation().Id().c_str();
        device.Close();
        return id;
    } catch (const winrt::hresult_error &) {
        return {};
    }
}
std::optional<bool> bluetooth_available(const std::atomic_bool *closing) {
    using namespace winrt::Windows::Devices::Radios;
    try {
        auto radios = detail::await_async([] { return Radio::GetRadiosAsync(); }, closing);
        bool unknown = false;
        for (const auto &radio : radios) {
            if (radio.Kind() != RadioKind::Bluetooth)
                continue;
            switch (radio.State()) {
            case RadioState::On:
                return true;
            case RadioState::Off:
            case RadioState::Disabled:
                break;
            default:
                unknown = true;
                break;
            }
        }
        return unknown ? std::nullopt : std::optional<bool>(false);
    } catch (...) {
        return std::nullopt;
    }
}
struct LocalPlaybackBackend::Impl {
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IAudioEndpointVolume> volume;
    Manager manager{nullptr};
    Session media{nullptr};
    std::wstring media_id;
    std::uint64_t generation{};
    GUID context{};
    const std::atomic_bool *closing{};
    explicit Impl(const std::atomic_bool *stop) : closing(stop) { checked(CoCreateGuid(&context)); }
    VolumeState read_volume(const std::wstring &selected) {
        VolumeState state;
        volume.Reset();
        if (selected.empty()) {
            state.message = L"请选择耳机；设备容器关联未确认时禁止调整其他输出。";
            return state;
        }
        using namespace winrt::Windows::Devices::Enumeration;
        auto properties = winrt::single_threaded_vector<winrt::hstring>({L"System.Devices.Aep.ContainerId"});
        auto device = detail::await_async([&] {
            return DeviceInformation::CreateFromIdAsync(selected, properties, DeviceInformationKind::AssociationEndpoint);
        }, closing);
        if (!device || !device.Properties().HasKey(L"System.Devices.Aep.ContainerId")) {
            state.message = L"耳机设备身份关联未确认。";
            return state;
        }
        auto boxed = device.Properties().Lookup(L"System.Devices.Aep.ContainerId");
        GUID expected{};
        if (auto p = boxed.try_as<winrt::Windows::Foundation::IPropertyValue>();
            p && p.Type() == winrt::Windows::Foundation::PropertyType::Guid)
            expected = p.GetGuid();
        else {
            auto value = winrt::unbox_value_or<winrt::hstring>(boxed, L"");
            if (FAILED(CLSIDFromString(value.c_str(), &expected))) {
                state.message = L"耳机容器标识无效。";
                return state;
            }
        }
        if (IsEqualGUID(expected, GUID_NULL)) {
            state.message = L"耳机容器标识为空。";
            return state;
        }
        if (!enumerator)
            checked(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(enumerator.GetAddressOf())));
        ComPtr<IMMDevice> current;
        checked(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, current.GetAddressOf()));
        DWORD status{};
        checked(current->GetState(&status));
        ComPtr<IPropertyStore> store;
        checked(current->OpenPropertyStore(STGM_READ, store.GetAddressOf()));
        PROPVARIANT container{};
        checked(store->GetValue(PKEY_Device_ContainerId, &container));
        bool match = container.vt == VT_CLSID && container.puuid && IsEqualGUID(*container.puuid, expected);
        PropVariantClear(&container);
        if (status != DEVICE_STATE_ACTIVE || !match) {
            state.message = L"耳机不是当前本机输出端点，音量操作已禁用。";
            return state;
        }
        LPWSTR id = nullptr;
        checked(current->GetId(&id));
        state.endpoint_id = id;
        CoTaskMemFree(id);
        PROPVARIANT name{};
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
            state.endpoint_name = name.pwszVal;
        PropVariantClear(&name);
        checked(current->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void **>(volume.GetAddressOf())));
        float scalar{};
        BOOL muted{};
        checked(volume->GetMasterVolumeLevelScalar(&scalar));
        checked(volume->GetMute(&muted));
        state.percent = scalar * 100.;
        state.muted = muted != FALSE;
        state.can_write = true;
        state.message = L"本机耳机输出端点；调节音量保持静音状态。";
        return state;
    }
    MediaState read_media() {
        MediaState state;
        state.message = L"仅控制本机系统媒体会话；耳机音源及手机播放状态未验证。";
        if (!manager)
            manager = detail::await_async([] { return Manager::RequestAsync(); }, closing);
        auto current = manager.GetCurrentSession();
        if (current != media) {
            media = current;
            media_id = current ? L"native-media-" + std::to_wstring(++generation) : L"";
        }
        if (!media)
            return state;
        auto target = media;
        auto properties = detail::await_async([&] { return target.TryGetMediaPropertiesAsync(); }, closing);
        if (manager.GetCurrentSession() != target) {
            media = nullptr;
            media_id.clear();
            state.message = L"本机媒体会话已更换，请重新读取。";
            return state;
        }
        auto info = target.GetPlaybackInfo();
        auto controls = info.Controls();
        state.session_id = media_id;
        state.title = properties.Title().c_str();
        state.application_id = target.SourceAppUserModelId().c_str();
        state.playing =
            info.PlaybackStatus() ==
            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        state.can_play = controls.IsPlayEnabled();
        state.can_pause = controls.IsPauseEnabled();
        state.can_previous = controls.IsPreviousEnabled();
        state.can_next = controls.IsNextEnabled();
        return state;
    }
};
LocalPlaybackBackend::LocalPlaybackBackend(const std::atomic_bool *closing) : impl_(std::make_unique<Impl>(closing)) {}
LocalPlaybackBackend::~LocalPlaybackBackend() = default;
LocalPlaybackSnapshot LocalPlaybackBackend::read(const std::wstring &selected) {
    LocalPlaybackSnapshot result;
    try {
        result.volume = impl_->read_volume(selected);
    } catch (const winrt::hresult_error &) {
        result.volume.message = L"本机耳机输出不可用，音量操作已禁用。";
    }
    try {
        result.media = impl_->read_media();
    } catch (const winrt::hresult_error &) {
        result.media.message = L"本机媒体会话不可用。";
        impl_->media = nullptr;
        impl_->media_id.clear();
    }
    return result;
}
DesktopResult LocalPlaybackBackend::set_volume(const std::wstring &selected, const std::wstring &expected,
                                               double percent) {
    if (!std::isfinite(percent) || percent < 0 || percent > 100)
        return {false, L"音量无效。"};
    try {
        auto state = impl_->read_volume(selected);
        if (!state.can_write || state.endpoint_id != expected || !impl_->volume)
            return {false, L"输出端点已变化，请重新读取。"};
        checked(
            impl_->volume->SetMasterVolumeLevelScalar(static_cast<float>(percent / 100), &impl_->context));
        return {true, L"已提交本机音量。"};
    } catch (const winrt::hresult_error &) {
        return {false, L"本机音量操作失败。"};
    }
}
DesktopResult LocalPlaybackBackend::execute_media(const std::wstring &expected, MediaAction action) {
    try {
        auto state = impl_->read_media();
        if (expected.empty() || state.session_id != expected || !impl_->media)
            return {false, L"本机媒体会话已更换。"};
        if (!state.allows(action))
            return {false, L"本机媒体会话不支持此操作。"};
        bool accepted = false;
        switch (action) {
        case MediaAction::play:
            accepted = detail::await_async([&] { return impl_->media.TryPlayAsync(); }, impl_->closing);
            break;
        case MediaAction::pause:
            accepted = detail::await_async([&] { return impl_->media.TryPauseAsync(); }, impl_->closing);
            break;
        case MediaAction::previous:
            accepted = detail::await_async([&] { return impl_->media.TrySkipPreviousAsync(); }, impl_->closing);
            break;
        case MediaAction::next:
            accepted = detail::await_async([&] { return impl_->media.TrySkipNextAsync(); }, impl_->closing);
            break;
        }
        return {accepted, accepted ? L"媒体会话已接受操作。" : L"本机媒体会话拒绝操作。"};
    } catch (const winrt::hresult_error &) {
        return {false, L"本机媒体操作失败。"};
    }
}

namespace {
std::vector<std::string> split(const std::string &value) {
    std::vector<std::string> result;
    std::istringstream input(value);
    for (std::string part; std::getline(input, part, '.');)
        result.push_back(part);
    return result;
}
bool digits(const std::string &value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool valid_gitee(const UpdateSource &s) {
    static const std::regex repo("^[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*$");
    return std::regex_match(s.gitee_repository, repo) &&
           s.gitee_release_page == "https://gitee.com/" + s.gitee_repository + "/releases" &&
           (!s.gitee_api.empty()
                ? s.gitee_api == "https://gitee.com/api/v5/repos/" + s.gitee_repository + "/releases"
                : s.gitee_manifest.starts_with("https://gitee.com/" + s.gitee_repository + "/raw/") &&
                  s.gitee_manifest.find_first_of("?#\\\r\n\t") == std::string::npos);
}
struct InternetHandle {
    HINTERNET value{};
    ~InternetHandle() {
        if (value)
            WinHttpCloseHandle(value);
    }
    operator HINTERNET() const { return value; }
};
UpdateHttpResponse get_https(const std::wstring &url, const std::wstring &token, const std::atomic_bool *canceled) {
    if (canceled && canceled->load())
        throw std::runtime_error("Canceled");
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = parts.dwUserNameLength =
        parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.nPort != 443 || parts.dwUserNameLength || parts.dwPasswordLength)
        throw std::runtime_error("Invalid update URL");
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
        path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) {
        // Only the service's bounded, anonymous Gitee pagination is allowed.
        // Keep arbitrary queries/fragments and authorization redirects blocked.
        const std::wstring query(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        static const std::wregex release_path(L"^/api/v5/repos/[A-Za-z0-9][A-Za-z0-9._-]*/[A-Za-z0-9][A-Za-z0-9._-]*/releases$");
        static const std::wregex page_query(L"^\\?page=([2-9]|10)&per_page=20&direction=asc$");
        if (host != L"gitee.com" || !token.empty() || !std::regex_match(path, release_path) ||
            !std::regex_match(query, page_query))
            throw std::runtime_error("Invalid update URL");
        path += query;
    }
    auto deadline = GetTickCount64() + 10000;
    auto remaining = [&]() -> int {
        if (canceled && canceled->load())
            throw std::runtime_error("Canceled");
        auto now = GetTickCount64();
        if (now >= deadline)
            throw std::runtime_error("Update timeout");
        return static_cast<int>(deadline - now);
    };
    InternetHandle session{WinHttpOpen(L"SonyConnect-Native", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session)
        throw std::runtime_error("WinHttpOpen");
    int timeout = remaining();
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    InternetHandle connection{WinHttpConnect(session, host.c_str(), 443, 0)};
    if (!connection)
        throw std::runtime_error("WinHttpConnect");
    InternetHandle request{WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request)
        throw std::runtime_error("WinHttpOpenRequest");
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)))
        throw std::runtime_error("Redirect policy");
    std::wstring headers = L"Accept: application/json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!token.empty()) {
        if (host != L"api.github.com" || token.find_first_of(L"\r\n") != std::wstring::npos)
            throw std::runtime_error("Invalid authorization");
        headers += L"Authorization: Bearer " + token + L"\r\n";
    }
    timeout = remaining();
    WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
    if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), nullptr, 0, 0, 0))
        throw std::runtime_error("Update request failed");
    timeout = remaining();
    WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
    if (!WinHttpReceiveResponse(request, nullptr))
        throw std::runtime_error("Update response failed");
    DWORD status{}, bytes = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &bytes, WINHTTP_NO_HEADER_INDEX))
        throw std::runtime_error("HTTP status missing");
    UpdateHttpResponse result{status, {}};
    std::array<char, 8192> buffer{};
    DWORD count{};
    for (;;) {
        timeout = remaining();
        WinHttpSetTimeouts(request, timeout, timeout, timeout, timeout);
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &count))
            throw std::runtime_error("Response failed");
        if (!count)
            break;
        if (result.body.size() + count > 262144)
            throw std::runtime_error("Release metadata exceeds limit");
        result.body.append(buffer.data(), count);
    }
    return result;
}
} // namespace
std::optional<ReleaseVersion> ReleaseVersion::parse(const std::string &text) {
    static const std::regex pattern("^v?(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+"
                                    "(?:\\.[0-9A-Za-z-]+)*))?(?:\\+[0-9A-Za-z-]+(?:\\.[0-9A-Za-z-]+)*)?$");
    std::smatch match;
    if (!std::regex_match(text, match, pattern))
        return {};
    try {
        ReleaseVersion result{std::stoi(match[1]), std::stoi(match[2]), std::stoi(match[3]), {}};
        if (match[4].matched) {
            result.prerelease = match[4].str();
            for (const auto &part : split(*result.prerelease))
                if (digits(part) && part.size() > 1 && part[0] == '0')
                    return {};
        }
        return result;
    } catch (const std::exception &) {
        return {};
    }
}
int ReleaseVersion::compare(const ReleaseVersion &other) const {
    for (auto [a, b] :
         {std::pair{major, other.major}, std::pair{minor, other.minor}, std::pair{patch, other.patch}})
        if (a != b)
            return a > b ? 1 : -1;
    if (!prerelease || !other.prerelease)
        return prerelease == other.prerelease ? 0 : !prerelease ? 1 : -1;
    auto a = split(*prerelease), b = split(*other.prerelease);
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        bool an = digits(a[i]), bn = digits(b[i]);
        int comparison = 0;
        if (an && bn && a[i].size() != b[i].size())
            comparison = a[i].size() > b[i].size() ? 1 : -1;
        else if (an != bn)
            comparison = an ? -1 : 1;
        else
            comparison = a[i].compare(b[i]);
        if (comparison)
            return comparison > 0 ? 1 : -1;
    }
    return a.size() == b.size() ? 0 : a.size() > b.size() ? 1 : -1;
}
UpdateSource UpdateSource::load(const std::filesystem::path &path) {
    return parse(read_file(path, 65536));
}
UpdateSource UpdateSource::parse(const std::string &text) {
    auto json = JsonObject::Parse(winrt::to_hstring(text));
    UpdateSource s;
    if (json.GetNamedNumber(L"schemaVersion") != 1)
        throw std::runtime_error("Unsupported update source schema");
    s.repository = winrt::to_string(json.GetNamedString(L"repository"));
    s.private_repository_requires_authentication =
        json.GetNamedBoolean(L"privateRepositoryRequiresAuthentication");
    if (json.GetNamedString(L"releasesPageUrl") != L"https://github.com/FLEETING-XHX/SonyConnect/releases" ||
        json.GetNamedString(L"latestStableReleaseApiUrl") !=
            L"https://api.github.com/repos/FLEETING-XHX/SonyConnect/releases/latest")
        throw std::runtime_error("Invalid update source");
    if (json.HasKey(L"gitee") &&
        json.GetNamedValue(L"gitee").ValueType() == winrt::Windows::Data::Json::JsonValueType::Object) {
        auto g = json.GetNamedObject(L"gitee");
        auto get = [&](const wchar_t *key) {
            auto v = g.GetNamedValue(key, JsonValue::CreateNullValue());
            return v.ValueType() == winrt::Windows::Data::Json::JsonValueType::String
                       ? winrt::to_string(v.GetString())
                       : std::string{};
        };
        s.gitee_repository = get(L"repository");
        s.gitee_release_page = get(L"releasesPageUrl");
        s.gitee_manifest = get(L"manifestUrl");
        s.gitee_api = get(L"latestReleasesApiUrl");
    }
    s.validate();
    return s;
}
void UpdateSource::validate() const {
    if (repository != "FLEETING-XHX/SonyConnect")
        throw std::invalid_argument("Invalid GitHub repository");
}
std::wstring UpdateSource::release_page(UpdateChannel channel) const {
    validate();
    return channel == UpdateChannel::github ? L"https://github.com/FLEETING-XHX/SonyConnect/releases"
           : valid_gitee(*this)             ? wide(gitee_release_page)
                                            : L"";
}
SoftwareUpdateService::SoftwareUpdateService(UpdateSource source, std::string current, Fetch fetch)
    : source_(std::move(source)), current_(std::move(current)), fetch_(fetch ? std::move(fetch) : Fetch{get_https}) {
    source_.validate();
}
UpdateResult SoftwareUpdateService::check(UpdateChannel channel, const std::wstring &token,
                                          const std::atomic_bool *canceled) const {
    if (canceled && canceled->load())
        return {UpdateStatus::canceled, L"已取消更新检测。", {}};
    if (channel == UpdateChannel::gitee &&
        (source_.gitee_repository.empty() || (source_.gitee_manifest.empty() && source_.gitee_api.empty()) ||
         source_.gitee_release_page.empty()))
        return {UpdateStatus::not_configured, L"Gitee 尚未配置版本清单。", {}};
    if (channel == UpdateChannel::gitee && !valid_gitee(source_))
        return {UpdateStatus::failed, L"Gitee 更新源配置不合法。", {}};
    if (channel == UpdateChannel::github && source_.private_repository_requires_authentication &&
        token.empty())
        return {UpdateStatus::needs_authorization, L"私有仓库需本次检查授权，或在 Releases 页面查看。", {}};
    try {
        auto response =
            fetch_(channel == UpdateChannel::github
                          ? L"https://api.github.com/repos/FLEETING-XHX/SonyConnect/releases/latest"
                          : wide(source_.gitee_api.empty() ? source_.gitee_manifest : source_.gitee_api),
                      channel == UpdateChannel::github ? token : L"", canceled);
        if (canceled && canceled->load()) return {UpdateStatus::canceled, L"已取消更新检测。", {}};
        if (response.status == 404) {
            if (channel == UpdateChannel::gitee)
                return {UpdateStatus::failed, L"Gitee 发布元数据不存在或不可访问。", {}};
            {
                auto repository =
                    fetch_(L"https://api.github.com/repos/FLEETING-XHX/SonyConnect", token, canceled);
                if (canceled && canceled->load()) return {UpdateStatus::canceled, L"已取消更新检测。", {}};
                if (repository.status < 200 || repository.status >= 300)
                    return {UpdateStatus::failed, L"仓库不可访问或授权不足。", {}};
            }
            return {UpdateStatus::no_release, L"尚无可比较的正式 Release。", {}};
        }
        if (channel == UpdateChannel::github && response.status == 403 &&
            response.body.find("API rate limit exceeded") != std::string::npos)
            return {UpdateStatus::failed, L"GitHub 匿名 API 请求已限流，请稍后重试或切换 Gitee 渠道。", {}};
        if (response.status < 200 || response.status >= 300)
            return {UpdateStatus::failed,
                    L"更新检测失败（HTTP " + std::to_wstring(response.status) + L"）。",
                    {}};
        if (response.body.size() > 262144)
            return {UpdateStatus::failed, L"发布元数据超过大小限制。", {}};
        std::string tag;
        if (channel == UpdateChannel::gitee && !source_.gitee_api.empty()) {
            auto releases = winrt::Windows::Data::Json::JsonArray::Parse(winrt::to_hstring(response.body));
            std::optional<ReleaseVersion> best;
            std::size_t metadata_bytes = response.body.size();
            // The first request uses Gitee's default ascending, 20-item page.
            // A full page is never enough to conclude that a version is current.
            for (unsigned page = 1;; ++page) {
                for (const auto &value : releases) {
                    auto release = value.GetObject();
                    if (release.GetNamedBoolean(L"prerelease", false) || release.GetNamedBoolean(L"draft", false)) continue;
                    const auto candidate = winrt::to_string(release.GetNamedString(L"tag_name"));
                    const auto parsed = ReleaseVersion::parse(candidate);
                    if (!parsed) return {UpdateStatus::failed, L"Gitee 正式版本格式无法比较。", {}};
                    if (parsed->prerelease) continue;
                    if (!best || parsed->compare(*best) > 0) { best = parsed; tag = candidate; }
                }
                if (releases.Size() < 20) break;
                if (page == 10)
                    return {UpdateStatus::failed, L"Gitee 版本列表超过检查上限，无法确认最新正式版本，请查看发行版页面。", {}};
                if (canceled && canceled->load()) return {UpdateStatus::canceled, L"已取消更新检测。", {}};
                response = fetch_(wide(source_.gitee_api) + L"?page=" + std::to_wstring(page + 1) +
                                      L"&per_page=20&direction=asc", L"", canceled);
                if (canceled && canceled->load()) return {UpdateStatus::canceled, L"已取消更新检测。", {}};
                if (response.status < 200 || response.status >= 300)
                    return {UpdateStatus::failed, L"Gitee 后续版本列表读取失败（HTTP " +
                                                     std::to_wstring(response.status) + L"），无法确认最新正式版本。", {}};
                metadata_bytes += response.body.size();
                if (response.body.size() > 262144 || metadata_bytes > 1048576)
                    return {UpdateStatus::failed, L"发布元数据超过大小限制，无法确认最新正式版本。", {}};
                releases = winrt::Windows::Data::Json::JsonArray::Parse(winrt::to_hstring(response.body));
            }
            if (!best) return {UpdateStatus::no_release, L"尚无可比较的正式 Release。", {}};
        } else {
        auto json = JsonObject::Parse(winrt::to_hstring(response.body));
        if (channel == UpdateChannel::github) {
            if (json.GetNamedBoolean(L"draft") || json.GetNamedBoolean(L"prerelease"))
                return {UpdateStatus::no_release, L"草稿或预发布未作为正式更新。", {}};
            tag = winrt::to_string(json.GetNamedString(L"tag_name"));
        } else {
            if (winrt::to_string(json.GetNamedString(L"releasePage")) != source_.gitee_release_page)
                return {UpdateStatus::failed, L"Gitee 清单发行版入口不一致。", {}};
            tag = winrt::to_string(json.GetNamedString(L"version"));
        }
        }
        auto latest = ReleaseVersion::parse(tag), current = ReleaseVersion::parse(current_);
        if (!latest || !current)
            return {UpdateStatus::failed, L"版本格式无法比较。", {}};
        if (latest->prerelease)
            return {UpdateStatus::no_release, L"预发布未作为正式更新。", {}};
        bool newer = latest->compare(*current) > 0;
        return {newer ? UpdateStatus::available : UpdateStatus::current,
                newer ? L"发现新版本 " + wide(tag) : L"当前版本不低于最新正式发布 " + wide(tag), wide(tag)};
    } catch (...) {
        return {canceled && canceled->load() ? UpdateStatus::canceled : UpdateStatus::failed,
                canceled && canceled->load() ? L"已取消更新检测。"
                                             : L"更新检测未完成：网络或发布元数据不可用。",
                {}};
    }
}

namespace {
constexpr std::size_t archive_limit = 128u * 1024u * 1024u;
constexpr std::size_t replay_limit = 100000;
std::string utc_now() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    char value[40]{};
    sprintf_s(value, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", time.wYear, time.wMonth, time.wDay, time.wHour,
              time.wMinute, time.wSecond, time.wMilliseconds);
    return value;
}
std::string run_prefix() {
    SYSTEMTIME time{};
    GetSystemTime(&time);
    GUID id{};
    checked(CoCreateGuid(&id));
    wchar_t guid[40]{};
    StringFromGUID2(id, guid, 40);
    std::string suffix;
    for (wchar_t c : std::wstring(guid))
        if (c != L'{' && c != L'}' && c != L'-')
            suffix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    char value[40]{};
    sprintf_s(value, "run-%04u%02u%02u-%02u%02u%02u%03u-", time.wYear, time.wMonth, time.wDay, time.wHour,
              time.wMinute, time.wSecond, time.wMilliseconds);
    return std::string(value) + suffix;
}
JsonObject event_json(const DiagnosticEvent &e) {
    JsonObject value;
    auto text = [&](const wchar_t *name, const std::string &data, std::size_t limit = 512) {
        if (!data.empty())
            value.SetNamedValue(
                name, JsonValue::CreateStringValue(winrt::to_hstring(DiagnosticLog::redact(data, limit))));
    };
    auto number = [&](const wchar_t *name, const auto &data) {
        if (data)
            value.SetNamedValue(name, JsonValue::CreateNumberValue(static_cast<double>(*data)));
    };
    text(L"Event", e.event);
    text(L"Status", e.status);
    text(L"Phase", e.phase);
    text(L"ErrorCode", e.error_code);
    text(L"Model", e.model, 128);
    text(L"Firmware", e.firmware, 128);
    text(L"Parameter", e.parameter);
    text(L"Origin", e.origin);
    text(L"SelectionId", e.selection_id);
    text(L"LinkId", e.link_id);
    text(L"RequestId", e.request_id);
    number(L"Type", e.type);
    number(L"Sequence", e.sequence);
    number(L"Command", e.command);
    number(L"PayloadBytes", e.payload_bytes);
    number(L"RequestedValue", e.requested_value);
    number(L"ConfirmedValue", e.confirmed_value);
    number(L"Generation", e.generation);
    number(L"ReceiveOrder", e.receive_order);
    return value;
}
std::string diagnostic_line(const DiagnosticEvent &event) {
    JsonObject root;
    root.SetNamedValue(L"Timestamp", JsonValue::CreateStringValue(winrt::to_hstring(utc_now())));
    root.SetNamedValue(L"Entry", event_json(event));
    return winrt::to_string(root.Stringify());
}
std::optional<std::int64_t> timestamp_ticks(const std::string &value) {
    static const std::regex pattern("^([0-9]{4})-([0-9]{2})-([0-9]{2})T([0-9]{2}):([0-9]{2}):([0-9]{2})(?:\\."
                                    "([0-9]{1,7}))?(Z|([+-])([0-9]{2}):([0-9]{2}))$");
    std::smatch match;
    if (!std::regex_match(value, match, pattern))
        return {};
    auto num = [&](int i) { return std::stoi(match[i]); };
    auto date = std::chrono::year{num(1)} / std::chrono::month{static_cast<unsigned>(num(2))} /
                std::chrono::day{static_cast<unsigned>(num(3))};
    int hour = num(4), minute = num(5), second = num(6);
    if (!date.ok() || num(1) < 1 || hour > 23 || minute > 59 || second > 59)
        return {};
    std::string fractional = match[7].matched ? match[7].str() : std::string{};
    fractional.resize(7, '0');
    std::int64_t fraction = std::stoll(fractional);
    std::int64_t seconds =
        std::chrono::sys_days(date).time_since_epoch().count() * 86400ll + hour * 3600 + minute * 60 + second;
    if (match[9].matched) {
        int off_hour = num(10), off_minute = num(11);
        if (off_hour > 23 || off_minute > 59)
            return {};
        int offset = off_hour * 3600 + off_minute * 60;
        seconds -= match[9] == "+" ? offset : -offset;
    }
    return seconds * 10000000ll + fraction;
}
std::optional<DiagnosticReplayEntry> parse_diagnostic_line(const std::string &line) {
    if (line.size() > 65536)
        return {};
    try {
        auto root = JsonObject::Parse(winrt::to_hstring(line));
        auto data = root.GetNamedObject(L"Entry");
        DiagnosticReplayEntry result;
        result.timestamp = winrt::to_string(root.GetNamedString(L"Timestamp"));
        // Invalid timestamps remain outside the timeline; this reader never invokes device services.
        if (!timestamp_ticks(result.timestamp))
            return {};
        auto text = [&](const wchar_t *name) {
            auto value = data.GetNamedValue(name, JsonValue::CreateNullValue());
            return value.ValueType() == winrt::Windows::Data::Json::JsonValueType::String
                       ? DiagnosticLog::redact(winrt::to_string(value.GetString()))
                       : std::string{};
        };
        auto number = [&](const wchar_t *name) -> std::optional<std::int64_t> {
            auto value = data.GetNamedValue(name, JsonValue::CreateNullValue());
            if (value.ValueType() != winrt::Windows::Data::Json::JsonValueType::Number)
                return {};
            double n = value.GetNumber();
            if (!std::isfinite(n) || n <= -9223372036854775808.0 || n >= 9223372036854775808.0 ||
                std::trunc(n) != n)
                return {};
            return static_cast<std::int64_t>(n);
        };
        auto integer = [&](const wchar_t *name) -> std::optional<int> {
            auto n = number(name);
            return n && *n >= INT_MIN && *n <= INT_MAX ? std::optional<int>(static_cast<int>(*n))
                                                       : std::nullopt;
        };
        auto &e = result.entry;
        e.event = text(L"Event");
        if (e.event.empty())
            return {};
        e.status = text(L"Status");
        e.phase = text(L"Phase");
        e.error_code = text(L"ErrorCode");
        e.model = text(L"Model");
        e.firmware = text(L"Firmware");
        e.parameter = text(L"Parameter");
        e.origin = text(L"Origin");
        e.selection_id = text(L"SelectionId");
        e.link_id = text(L"LinkId");
        e.request_id = text(L"RequestId");
        e.type = integer(L"Type");
        e.sequence = integer(L"Sequence");
        e.command = integer(L"Command");
        e.payload_bytes = integer(L"PayloadBytes");
        e.requested_value = integer(L"RequestedValue");
        e.confirmed_value = integer(L"ConfirmedValue");
        e.generation = number(L"Generation");
        e.receive_order = number(L"ReceiveOrder");
        return result;
    } catch (const winrt::hresult_error &) {
        return {};
    }
}
std::string safe_line(const std::string &line) {
    auto entry = parse_diagnostic_line(line);
    if (!entry)
        return {};
    JsonObject root;
    root.SetNamedValue(L"Timestamp", JsonValue::CreateStringValue(winrt::to_hstring(entry->timestamp)));
    root.SetNamedValue(L"Entry", event_json(entry->entry));
    return winrt::to_string(root.Stringify());
}
bool owned_log(const std::filesystem::path &path) {
    static const std::regex pattern("^run-[0-9]{8}-[0-9]{9}-[0-9a-f]{32}-[0-3]\\.jsonl$");
    return std::regex_match(path.filename().string(), pattern);
}
std::uint32_t crc32(const std::string &value) {
    std::uint32_t crc = 0xffffffffu;
    for (unsigned char byte : value) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return ~crc;
}
void u16(std::string &out, std::uint16_t value) {
    out.push_back(static_cast<char>(value));
    out.push_back(static_cast<char>(value >> 8));
}
void u32(std::string &out, std::uint32_t value) {
    u16(out, static_cast<std::uint16_t>(value));
    u16(out, static_cast<std::uint16_t>(value >> 16));
}
std::uint16_t r16(const std::string &in, std::size_t offset) {
    if (offset > in.size() || in.size() - offset < 2)
        throw std::runtime_error("Truncated ZIP");
    return static_cast<unsigned char>(in[offset]) | (static_cast<unsigned char>(in[offset + 1]) << 8);
}
std::uint32_t r32(const std::string &in, std::size_t offset) {
    return r16(in, offset) | (static_cast<std::uint32_t>(r16(in, offset + 2)) << 16);
}
struct ZipItem {
    std::string name, data;
};
std::string zip_store(const std::vector<ZipItem> &files) {
    std::string output, central;
    for (const auto &f : files) {
        auto offset = static_cast<std::uint32_t>(output.size()),
             length = static_cast<std::uint32_t>(f.data.size()), crc = crc32(f.data);
        u32(output, 0x04034b50);
        u16(output, 20);
        u16(output, 0x800);
        u16(output, 0);
        u16(output, 0);
        u16(output, 0);
        u32(output, crc);
        u32(output, length);
        u32(output, length);
        u16(output, static_cast<std::uint16_t>(f.name.size()));
        u16(output, 0);
        output += f.name;
        output += f.data;
        u32(central, 0x02014b50);
        u16(central, 20);
        u16(central, 20);
        u16(central, 0x800);
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u32(central, crc);
        u32(central, length);
        u32(central, length);
        u16(central, static_cast<std::uint16_t>(f.name.size()));
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u16(central, 0);
        u32(central, 0);
        u32(central, offset);
        central += f.name;
    }
    auto offset = static_cast<std::uint32_t>(output.size());
    output += central;
    u32(output, 0x06054b50);
    u16(output, 0);
    u16(output, 0);
    u16(output, static_cast<std::uint16_t>(files.size()));
    u16(output, static_cast<std::uint16_t>(files.size()));
    u32(output, static_cast<std::uint32_t>(central.size()));
    u32(output, offset);
    u16(output, 0);
    return output;
}
// Bounded RFC 1951 decoder, used only for .NET ZipArchive diagnostic imports.
// Supports stored, fixed and dynamic blocks; encrypted and ZIP64 archives are rejected.
struct BitReader {
    const std::string &value;
    std::size_t byte{};
    unsigned bit{};
    unsigned read(unsigned count) {
        unsigned result = 0;
        for (unsigned i = 0; i < count; ++i) {
            if (byte >= value.size())
                throw std::runtime_error("Truncated DEFLATE");
            result |= ((static_cast<unsigned char>(value[byte]) >> bit) & 1u) << i;
            if (++bit == 8) {
                bit = 0;
                ++byte;
            }
        }
        return result;
    }
    void align() {
        if (bit) {
            bit = 0;
            ++byte;
        }
    }
};
struct Huffman {
    std::array<std::map<unsigned, unsigned>, 16> codes;
    explicit Huffman(const std::vector<unsigned> &lengths) {
        std::array<unsigned, 16> counts{}, next{};
        for (auto n : lengths) {
            if (n > 15)
                throw std::runtime_error("Invalid Huffman length");
            if (n)
                ++counts[n];
        }
        int left = 1;
        for (unsigned len = 1; len <= 15; ++len) {
            left = (left << 1) - static_cast<int>(counts[len]);
            if (left < 0)
                throw std::runtime_error("Oversubscribed Huffman tree");
        }
        unsigned code = 0;
        for (unsigned len = 1; len <= 15; ++len) {
            code = (code + counts[len - 1]) << 1;
            next[len] = code;
        }
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            unsigned len = lengths[symbol];
            if (!len)
                continue;
            unsigned c = next[len]++, reverse = 0;
            for (unsigned i = 0; i < len; ++i) {
                reverse = (reverse << 1) | (c & 1);
                c >>= 1;
            }
            codes[len][reverse] = symbol;
        }
    }
    unsigned decode(BitReader &input) const {
        unsigned code = 0;
        for (unsigned len = 1; len <= 15; ++len) {
            code |= input.read(1) << (len - 1);
            auto found = codes[len].find(code);
            if (found != codes[len].end())
                return found->second;
        }
        throw std::runtime_error("Invalid Huffman code");
    }
};
std::string inflate_raw(const std::string &compressed, std::size_t expected) {
    if (expected > archive_limit)
        throw std::runtime_error("Diagnostic archive exceeds limit");
    BitReader input{compressed};
    std::string output;
    output.reserve(expected);
    bool final = false;
    static constexpr unsigned length_base[] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                               15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                               67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr unsigned length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr unsigned distance_base[] = {
        1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr unsigned distance_extra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    while (!final) {
        final = input.read(1) != 0;
        unsigned type = input.read(2);
        if (type == 0) {
            input.align();
            unsigned len = input.read(16), inverse = input.read(16);
            if ((len ^ inverse) != 65535 || output.size() + len > expected)
                throw std::runtime_error("Invalid stored block");
            for (unsigned i = 0; i < len; ++i)
                output.push_back(static_cast<char>(input.read(8)));
            continue;
        }
        std::vector<unsigned> literal, distance;
        if (type == 1) {
            literal.resize(288);
            for (unsigned i = 0; i < 288; ++i)
                literal[i] = i <= 143 ? 8 : i <= 255 ? 9 : i <= 279 ? 7 : 8;
            distance.assign(32, 5);
        } else if (type == 2) {
            unsigned hlit = input.read(5) + 257, hdist = input.read(5) + 1, hclen = input.read(4) + 4;
            if (hlit > 286)
                throw std::runtime_error("Invalid dynamic block");
            static constexpr unsigned correct_order[] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                         11, 4,  12, 3, 13, 2, 14, 1, 15};
            std::vector<unsigned> code_lengths(19);
            for (unsigned i = 0; i < hclen; ++i)
                code_lengths[correct_order[i]] = input.read(3);
            Huffman code_tree(code_lengths);
            std::vector<unsigned> lengths;
            while (lengths.size() < hlit + hdist) {
                auto symbol = code_tree.decode(input);
                if (symbol < 16) {
                    lengths.push_back(symbol);
                    continue;
                }
                unsigned count = 0, value = 0;
                if (symbol == 16) {
                    if (lengths.empty())
                        throw std::runtime_error("Invalid repeat");
                    value = lengths.back();
                    count = input.read(2) + 3;
                } else if (symbol == 17)
                    count = input.read(3) + 3;
                else if (symbol == 18)
                    count = input.read(7) + 11;
                else
                    throw std::runtime_error("Invalid code length");
                if (lengths.size() + count > hlit + hdist)
                    throw std::runtime_error("Code lengths overflow");
                lengths.insert(lengths.end(), count, value);
            }
            literal.assign(lengths.begin(), lengths.begin() + hlit);
            distance.assign(lengths.begin() + hlit, lengths.end());
            if (!literal[256])
                throw std::runtime_error("Missing end code");
        } else
            throw std::runtime_error("Reserved DEFLATE block");
        Huffman literals(literal), distances(distance);
        for (;;) {
            unsigned symbol = literals.decode(input);
            if (symbol == 256)
                break;
            if (symbol < 256) {
                if (output.size() >= expected)
                    throw std::runtime_error("DEFLATE output exceeds declared size");
                output.push_back(static_cast<char>(symbol));
                continue;
            }
            if (symbol < 257 || symbol > 285)
                throw std::runtime_error("Invalid length code");
            unsigned index = symbol - 257, len = length_base[index] + input.read(length_extra[index]),
                     d = distances.decode(input);
            if (d >= 30)
                throw std::runtime_error("Invalid distance code");
            unsigned offset = distance_base[d] + input.read(distance_extra[d]);
            if (offset > output.size() || output.size() + len > expected)
                throw std::runtime_error("Invalid DEFLATE back reference");
            for (unsigned i = 0; i < len; ++i)
                output.push_back(output[output.size() - offset]);
        }
    }
    if (output.size() != expected)
        throw std::runtime_error("DEFLATE size mismatch");
    if (input.byte + (input.bit ? 1u : 0u) != compressed.size())
        throw std::runtime_error("Trailing DEFLATE data");
    return output;
}
std::vector<ZipItem> zip_read(const std::string &archive) {
    if (archive.size() < 22)
        throw std::runtime_error("Invalid diagnostic ZIP");
    std::size_t eocd = std::string::npos;
    std::size_t lower = archive.size() > 65557 ? archive.size() - 65557 : 0;
    for (std::size_t pos = archive.size() - 22;; --pos) {
        if (r32(archive, pos) == 0x06054b50 && pos + 22 + r16(archive, pos + 20) == archive.size()) {
            eocd = pos;
            break;
        }
        if (pos == lower)
            break;
    }
    if (eocd == std::string::npos || r16(archive, eocd + 4) || r16(archive, eocd + 6))
        throw std::runtime_error("Unsupported ZIP layout");
    unsigned count = r16(archive, eocd + 10);
    if (count != r16(archive, eocd + 8) || count > 401)
        throw std::runtime_error("Too many diagnostic files");
    std::size_t central = r32(archive, eocd + 16), central_size = r32(archive, eocd + 12);
    if (central > eocd || central_size != eocd - central)
        throw std::runtime_error("Invalid ZIP directory");
    std::vector<ZipItem> result;
    std::set<std::string> names;
    std::size_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (r32(archive, central) != 0x02014b50 || central + 46 > eocd)
            throw std::runtime_error("Invalid ZIP entry");
        unsigned flags = r16(archive, central + 8), method = r16(archive, central + 10);
        auto crc = r32(archive, central + 16);
        std::size_t size = r32(archive, central + 24), packed = r32(archive, central + 20),
                    name_length = r16(archive, central + 28), extra = r16(archive, central + 30),
                    comment = r16(archive, central + 32), local = r32(archive, central + 42);
        if ((flags & 1) || method != 0 && method != 8 || r16(archive, central + 34) || size == 0xffffffffu ||
            packed == 0xffffffffu || central + 46 + name_length + extra + comment > eocd)
            throw std::runtime_error("Unsupported ZIP entry");
        std::string name = archive.substr(central + 46, name_length);
        if (!names.insert(name).second)
            throw std::runtime_error("Duplicate ZIP entry");
        central += 46 + name_length + extra + comment;
        bool wanted = name == "manifest.json" || (name.starts_with("logs/") && name.ends_with(".jsonl"));
        if (!wanted)
            continue;
        if (size > archive_limit || total + size > archive_limit || (name == "manifest.json" && size > 65536))
            throw std::runtime_error("Diagnostic archive exceeds limit");
        total += size;
        if (local >= r32(archive, eocd + 16) || r32(archive, local) != 0x04034b50)
            throw std::runtime_error("Invalid ZIP local entry");
        std::size_t start = local + 30 + r16(archive, local + 26) + r16(archive, local + 28);
        if (start > r32(archive, eocd + 16) || packed > r32(archive, eocd + 16) - start)
            throw std::runtime_error("ZIP entry out of range");
        auto data = archive.substr(start, packed);
        if (method == 8)
            data = inflate_raw(data, size);
        if (data.size() != size || crc32(data) != crc)
            throw std::runtime_error("Diagnostic ZIP checksum mismatch");
        result.push_back({name, std::move(data)});
    }
    return result;
}
} // namespace
void DiagnosticOptions::validate() const {
    if (segment_limit_mib < 1 || segment_limit_mib > 8 || retained_runs < 1 || retained_runs > 100)
        throw std::invalid_argument("Diagnostic options out of range");
}
std::string DiagnosticLog::redact(std::string value, std::size_t limit) {
    static const std::regex address("(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}", std::regex::icase),
        identifier("[0-9a-f]{12}", std::regex::icase);
    value = std::regex_replace(value, address, "[redacted]");
    auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    std::string clean;
    std::size_t copied = 0;
    for (std::sregex_iterator it(value.begin(), value.end(), identifier), end; it != end; ++it) {
        auto position = static_cast<std::size_t>(it->position()), after = position + 12;
        if ((position && hex(value[position - 1])) || (after < value.size() && hex(value[after])))
            continue;
        clean.append(value, copied, position - copied);
        clean += "[redacted]";
        copied = after;
    }
    clean.append(value, copied, std::string::npos);
    value = std::move(clean);
    if (value.size() > limit) {
        std::size_t cut = limit;
        while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xc0) == 0x80)
            --cut;
        value.resize(cut);
    }
    return value;
}
struct DiagnosticLog::Impl {
    std::filesystem::path directory;
    std::string prefix;
    DiagnosticOptions options;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<DiagnosticEvent> queue;
    std::thread writer;
    bool stopping{}, writing{};
    std::size_t dropped{};
    std::wstring failure;
    std::mutex files_mutex;
    std::condition_variable drained;
    winrt::file_handle run_lease;
    Impl(std::filesystem::path dir, DiagnosticOptions opts)
        : directory(std::filesystem::absolute(dir)), prefix(run_prefix()), options(opts) {
        options.validate();
        std::filesystem::create_directories(directory);
        // Keep a per-run lease open for the entire writer lifetime. Windows
        // releases it after a crash too; no end-of-log marker is needed.
        run_lease.attach(CreateFileW(lease_path(prefix).c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!run_lease) winrt::throw_last_error();
        {
            std::ofstream initial(path(0), std::ios::binary | std::ios::trunc);
            if (!initial)
                throw std::runtime_error("Cannot create diagnostic log");
        }
        prune();
        writer = std::thread([this] { run(); });
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (writer.joinable())
            writer.join();
        run_lease.close();
        std::error_code ec;
        std::filesystem::remove(lease_path(prefix), ec);
    }
    std::filesystem::path path(unsigned segment) const {
        return directory / (prefix + "-" + std::to_string(segment) + ".jsonl");
    }
    std::filesystem::path lease_path(const std::string &name) const { return directory / (name + ".lock"); }
    bool run_active(const std::string &name) const {
        winrt::file_handle probe{CreateFileW(lease_path(name).c_str(), GENERIC_READ | GENERIC_WRITE,
                                        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (probe) return false;
        const auto error = GetLastError();
        // Missing leases belong to completed runs or pre-lease versions. The
        // application single-instance guard prevents an old product writer
        // from running alongside this version. Unknown access errors retain data.
        return error != ERROR_FILE_NOT_FOUND;
    }
    void prune() {
        std::lock_guard file_lock(files_mutex);
        std::map<std::string, std::vector<std::filesystem::path>, std::greater<>> runs;
        for (const auto &entry : std::filesystem::directory_iterator(directory))
            if (entry.is_regular_file() && owned_log(entry.path())) {
                auto name = entry.path().filename().string();
                runs[name.substr(0, name.size() - 8)].push_back(entry.path());
            }
        unsigned keep;
        {
            std::lock_guard lock(mutex);
            keep = options.retained_runs;
        }
        unsigned active = 0;
        std::vector<std::vector<std::filesystem::path>> closed;
        for (const auto &[name, files] : runs) {
            if (name == prefix || run_active(name)) {
                ++active;
                continue;
            }
            closed.push_back(files);
        }
        unsigned completed_keep = keep > active ? keep - active : 0;
        for (std::size_t i = completed_keep; i < closed.size(); ++i) {
            bool removed = true;
            for (const auto &file : closed[i]) {
                std::error_code ec;
                std::filesystem::remove(file, ec);
                removed &= !ec;
            }
            if (removed) {
                auto name = closed[i].front().filename().string();
                std::error_code ec;
                std::filesystem::remove(lease_path(name.substr(0, name.size() - 8)), ec);
            }
        }
    }
    void run() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            unsigned segment = 0;
            std::size_t written = 0;
            for (;;) {
                DiagnosticEvent event;
                std::size_t limit{};
                {
                    std::unique_lock lock(mutex);
                    wake.wait(lock, [&] { return stopping || !queue.empty(); });
                    if (queue.empty() && stopping)
                        break;
                    event = std::move(queue.front());
                    queue.pop_front();
                    writing = true;
                    limit = options.segment_limit_mib * 1024u * 1024u;
                }
                auto line = diagnostic_line(event) + "\n";
                {
                    std::lock_guard file_lock(files_mutex);
                    bool rotate = written + line.size() > limit;
                    if (rotate) {
                        segment = (segment + 1) % 4;
                        written = 0;
                    }
                    std::ofstream output(path(segment),
                                         std::ios::binary | (rotate ? std::ios::trunc : std::ios::app));
                    output.write(line.data(), line.size());
                    output.flush();
                    if (!output)
                        throw std::runtime_error("Diagnostic write failed");
                    written += line.size();
                }
                {
                    std::lock_guard lock(mutex);
                    writing = false;
                }
                drained.notify_all();
            }
            DiagnosticEvent ended;
            ended.event = "log-end";
            {
                std::lock_guard lock(mutex);
                ended.status = "dropped=" + std::to_string(dropped);
            }
            auto line = diagnostic_line(ended) + "\n";
            {
                std::lock_guard file_lock(files_mutex);
                std::ofstream output(path(segment), std::ios::binary | std::ios::app);
                output.write(line.data(), line.size());
                output.flush();
                if (!output)
                    throw std::runtime_error("Diagnostic close failed");
            }
            winrt::uninit_apartment();
        } catch (...) {
            std::lock_guard lock(mutex);
            failure = L"诊断日志写入失败。";
            stopping = true;
            writing = false;
            queue.clear();
            drained.notify_all();
        }
    }
};
DiagnosticLog::DiagnosticLog(std::filesystem::path dir, DiagnosticOptions opts)
    : impl_(std::make_unique<Impl>(std::move(dir), opts)) {
    DiagnosticEvent start;
    start.event = "run-start";
    start.status = "native;hardware-validation=not-asserted";
    record(std::move(start));
}
DiagnosticLog::~DiagnosticLog() = default;
void DiagnosticLog::record(DiagnosticEvent e) {
    e.event = redact(std::move(e.event));
    e.status = redact(std::move(e.status));
    e.phase = redact(std::move(e.phase));
    e.error_code = redact(std::move(e.error_code));
    e.model = redact(std::move(e.model), 128);
    e.firmware = redact(std::move(e.firmware), 128);
    e.parameter = redact(std::move(e.parameter));
    e.origin = redact(std::move(e.origin));
    e.selection_id = redact(std::move(e.selection_id));
    e.link_id = redact(std::move(e.link_id));
    e.request_id = redact(std::move(e.request_id));
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || impl_->queue.size() >= 2048) {
            ++impl_->dropped;
            return;
        }
        impl_->queue.push_back(std::move(e));
    }
    impl_->wake.notify_one();
}
void DiagnosticLog::apply_options(DiagnosticOptions options) {
    options.validate();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->options = options;
    }
    impl_->prune();
    DiagnosticEvent e;
    e.event = "diagnostic-options-updated";
    e.status = "segmentMiB=" + std::to_string(options.segment_limit_mib) +
               ";retainedRuns=" + std::to_string(options.retained_runs);
    record(std::move(e));
}
std::wstring DiagnosticLog::status() const {
    std::lock_guard lock(impl_->mutex);
    return !impl_->failure.empty() ? impl_->failure
           : impl_->dropped        ? L"日志队列已丢弃 " + std::to_wstring(impl_->dropped) + L" 条事件。"
                                   : L"自动日志已开启。";
}
void DiagnosticLog::export_archive(const std::filesystem::path &destination) {
    auto target = std::filesystem::absolute(destination).lexically_normal();
    std::error_code same_error;
    if (std::filesystem::equivalent(target.parent_path(), impl_->directory, same_error) &&
        _wcsicmp(target.extension().c_str(), L".jsonl") == 0)
        throw std::invalid_argument("Diagnostic export cannot overwrite a source log");
    {
        std::unique_lock lock(impl_->mutex);
        impl_->drained.wait(lock,
                            [&] { return impl_->stopping || (impl_->queue.empty() && !impl_->writing); });
        if (!impl_->failure.empty())
            throw std::runtime_error("Diagnostic writer failed");
    }
    std::vector<ZipItem> entries;
    JsonObject manifest;
    manifest.SetNamedValue(L"Schema", JsonValue::CreateStringValue(L"SonyConnect.Diagnostics.v1"));
    manifest.SetNamedValue(L"ExportedAt", JsonValue::CreateStringValue(winrt::to_hstring(utc_now())));
    manifest.SetNamedValue(L"HardwareValidation",
                           JsonValue::CreateStringValue(L"not-asserted-by-diagnostic-log"));
    JsonArray redactions;
    redactions.Append(JsonValue::CreateStringValue(L"Bluetooth addresses"));
    redactions.Append(JsonValue::CreateStringValue(L"12-digit device identifiers"));
    redactions.Append(JsonValue::CreateStringValue(L"unrecognized event fields"));
    manifest.SetNamedValue(L"Redactions", redactions);
    entries.push_back({"manifest.json", winrt::to_string(manifest.Stringify())});
    std::size_t total = 0;
    std::lock_guard file_lock(impl_->files_mutex);
    std::vector<std::filesystem::path> files;
    for (const auto &file : std::filesystem::directory_iterator(impl_->directory))
        if (file.is_regular_file() && owned_log(file.path()))
            files.push_back(file.path());
    std::sort(files.begin(), files.end());
    if (files.size() > 400)
        throw std::runtime_error("Too many diagnostic files");
    for (const auto &file : files) {
        auto data = read_file(file, 8 * 1024 * 1024 + 65536);
        total += data.size();
        if (total > archive_limit)
            throw std::runtime_error("Diagnostic export exceeds limit");
        std::istringstream input(data);
        std::string line, clean;
        while (std::getline(input, line)) {
            auto sanitized = safe_line(line);
            if (!sanitized.empty())
                clean += sanitized + "\n";
        }
        entries.push_back({"logs/" + file.filename().string(), std::move(clean)});
    }
    atomic_write(destination, zip_store(entries), false);
}
std::vector<DiagnosticReplayEntry> DiagnosticLog::read_replay(const std::filesystem::path &archive) {
    auto files = zip_read(read_file(archive, archive_limit + 4 * 1024 * 1024));
    auto manifest =
        std::find_if(files.begin(), files.end(), [](const auto &f) { return f.name == "manifest.json"; });
    if (manifest == files.end())
        throw std::runtime_error("Diagnostic manifest missing");
    if (JsonObject::Parse(winrt::to_hstring(manifest->data)).GetNamedString(L"Schema") !=
        L"SonyConnect.Diagnostics.v1")
        throw std::runtime_error("Unsupported diagnostic schema");
    std::vector<DiagnosticReplayEntry> result;
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    for (const auto &file : files)
        if (file.name.starts_with("logs/")) {
            std::istringstream input(file.data);
            std::string line;
            while (std::getline(input, line)) {
                if (result.size() >= replay_limit)
                    throw std::runtime_error("Too many diagnostic events");
                auto e = parse_diagnostic_line(line);
                if (e)
                    result.push_back(std::move(*e));
            }
        }
    std::stable_sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
        return *timestamp_ticks(a.timestamp) < *timestamp_ticks(b.timestamp);
    });
    return result;
}

} // namespace sonyconnect::desktop
