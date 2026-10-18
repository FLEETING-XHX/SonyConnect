#include "sonyconnect/desktop.hpp"
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace d = sonyconnect::desktop;
namespace j = winrt::Windows::Data::Json;
void check(bool ok, const char *text) { if (!ok) throw std::runtime_error(text); }
int wmain(int argc, wchar_t **argv) {
    try {
        winrt::init_apartment();
        if (argc == 4 && std::wstring(argv[1]) == L"--live") {
            const auto source = d::UpdateSource::load(argv[2]);
            j::JsonArray results;
            bool ok = true;
            for (const auto channel : {d::UpdateChannel::github, d::UpdateChannel::gitee}) {
                const auto r = d::SoftwareUpdateService(source, SONYCONNECT_VERSION).check(channel);
                j::JsonObject entry;
                entry.SetNamedValue(L"channel", j::JsonValue::CreateStringValue(channel == d::UpdateChannel::github ? L"GitHub" : L"Gitee"));
                entry.SetNamedValue(L"status", j::JsonValue::CreateNumberValue(static_cast<int>(r.status)));
                entry.SetNamedValue(L"message", j::JsonValue::CreateStringValue(r.message));
                entry.SetNamedValue(L"latest", j::JsonValue::CreateStringValue(r.latest));
                results.Append(entry);
                ok &= r.status == d::UpdateStatus::no_release || r.status == d::UpdateStatus::current || r.status == d::UpdateStatus::available;
            }
            std::ofstream out{std::filesystem::path(argv[3])};
            out << winrt::to_string(results.Stringify());
            std::cout << winrt::to_string(results.Stringify()) << '\n';
            return ok ? 0 : 1;
        }
        unsigned passed{};
        d::UpdateSource source;
        source.private_repository_requires_authentication = false;
        source.gitee_repository = "fleetingxhx/SonyConnect";
        source.gitee_release_page = "https://gitee.com/fleetingxhx/SonyConnect/releases";
        source.gitee_api = "https://gitee.com/api/v5/repos/fleetingxhx/SonyConnect/releases";
        const auto run = [&](d::UpdateChannel channel, unsigned http, std::string body) {
            return d::SoftwareUpdateService(source, "0.0.3", [&, http, body](const auto &url, const auto &token, auto *) {
                if (channel == d::UpdateChannel::gitee) {
                    check(url == std::wstring(source.gitee_api.begin(), source.gitee_api.end()) && token.empty(), "Gitee must use exact anonymous API");
                }
                return d::UpdateHttpResponse{http, body};
            }).check(channel);
        };
        auto r = run(d::UpdateChannel::gitee, 200, "[]");
        check(r.status == d::UpdateStatus::no_release, "Empty Gitee list is no release, never current"); ++passed;
        r = run(d::UpdateChannel::gitee, 200, R"([{"tag_name":"v0.0.4","prerelease":false}])");
        check(r.status == d::UpdateStatus::available && r.latest == L"v0.0.4", "Higher Gitee version must be available"); ++passed;
        r = run(d::UpdateChannel::gitee, 200, R"([{"tag_name":"v0.0.3"},{"tag_name":"v0.0.2"}])");
        check(r.status == d::UpdateStatus::current, "Highest stable version must be compared"); ++passed;
        r = run(d::UpdateChannel::gitee, 200, R"([{"tag_name":"v2.0.0-rc.1"},{"tag_name":"v1.0.0","prerelease":true},{"tag_name":"v0.0.4"}])");
        check(r.latest == L"v0.0.4", "Prereleases must be filtered before comparison"); ++passed;
        for (const auto http : {403u, 404u, 429u, 500u}) {
            check(run(d::UpdateChannel::gitee, http, "{}").status == d::UpdateStatus::failed, "HTTP errors must not mean current"); ++passed;
        }
        for (const std::string body : {"{}", "not json", "[{\"tag_name\":\"bad\"}]", "[{\"tag_name\":true}]"}) {
            check(run(d::UpdateChannel::gitee, 200, body).status == d::UpdateStatus::failed, "Invalid schema/version must fail"); ++passed;
        }
        check(run(d::UpdateChannel::gitee, 200, std::string(262145, ' ')).status == d::UpdateStatus::failed, "Metadata must remain bounded"); ++passed;
        r = run(d::UpdateChannel::github, 200, R"({"tag_name":"v0.0.4","draft":false,"prerelease":false})");
        check(r.status == d::UpdateStatus::available, "GitHub stable higher version must be available"); ++passed;
        r = run(d::UpdateChannel::github, 200, R"({"tag_name":"v0.0.4","draft":true,"prerelease":false})");
        check(r.status == d::UpdateStatus::no_release, "Draft must not be an update"); ++passed;
        unsigned requests{};
        r = run(d::UpdateChannel::github, 403, R"({"message":"API rate limit exceeded"})");
        check(r.status == d::UpdateStatus::failed && r.message.find(L"Gitee") != std::wstring::npos, "GitHub rate limit must suggest alternate channel"); ++passed;
        r = d::SoftwareUpdateService(source, "0.0.3", [&](const auto &url, const auto &, auto *) {
            ++requests;
            return d::UpdateHttpResponse{url.ends_with(L"/latest") ? 404u : 200u, "{}"};
        }).check(d::UpdateChannel::github);
        check(r.status == d::UpdateStatus::no_release && requests == 2, "GitHub 404 must confirm repository access"); ++passed;
        check(run(d::UpdateChannel::github, 404, "{}").status == d::UpdateStatus::failed, "Unavailable GitHub repository must not mean no release"); ++passed;
        std::atomic_bool canceled{false};
        r = d::SoftwareUpdateService(source, "0.0.3", [&](const auto &, const auto &, auto *) {
            canceled = true;
            return d::UpdateHttpResponse{200, "[]"};
        }).check(d::UpdateChannel::gitee, L"", &canceled);
        check(r.status == d::UpdateStatus::canceled, "Cancellation during request must suppress success"); ++passed;
        r = d::SoftwareUpdateService(source, "0.0.3", [](const auto &, const auto &, auto *) -> d::UpdateHttpResponse {
            throw std::runtime_error("fixture offline/timeout/TLS failure");
        }).check(d::UpdateChannel::github);
        check(r.status == d::UpdateStatus::failed, "Transport failures must not be current"); ++passed;
        const auto full_page = [](bool prerelease, bool padded = false) {
            std::string body = "[";
            for (int i = 0; i < 20; ++i)
                body += (i ? "," : "") + std::string(prerelease
                    ? R"({"tag_name":"v9.0.0-rc.1","prerelease":true})"
                    : R"({"tag_name":"v0.0.3"})");
            body += "]";
            if (padded) body.resize(262144, ' ');
            return body;
        };
        const auto base_url = std::wstring(source.gitee_api.begin(), source.gitee_api.end());
        const auto paginated = [&](const std::string &first, unsigned later_http, const std::string &later,
                                   unsigned &calls, std::atomic_bool *stop = nullptr) {
            calls = 0;
            return d::SoftwareUpdateService(source, "0.0.3", [&](const auto &url, const auto &token, auto *) {
                ++calls;
                const auto expected = calls == 1 ? base_url
                    : base_url + L"?page=" + std::to_wstring(calls) + L"&per_page=20&direction=asc";
                check(url == expected && token.empty(), "Pagination must stay on the pinned anonymous endpoint");
                if (calls == 1) return d::UpdateHttpResponse{200, first};
                if (stop) *stop = true;
                return d::UpdateHttpResponse{later_http, later};
            }).check(d::UpdateChannel::gitee, L"", stop);
        };
        r = paginated(full_page(false), 200, R"([{"tag_name":"v0.0.4"}])", requests);
        check(r.status == d::UpdateStatus::available && r.latest == L"v0.0.4" && requests == 2,
              "Later-page stable release must not be hidden by an older first page"); ++passed;
        for (const auto &body : {std::string("{}"), std::string("invalid")}) {
            r = paginated(full_page(false), 200, body, requests);
            check(r.status == d::UpdateStatus::failed && requests == 2, "Invalid later page must not mean current"); ++passed;
        }
        r = paginated(full_page(false), 429, "{}", requests);
        check(r.status == d::UpdateStatus::failed && requests == 2, "Later-page HTTP failure must not mean current"); ++passed;
        r = paginated(full_page(true), 200, R"([{"tag_name":"v0.0.4"}])", requests);
        check(r.status == d::UpdateStatus::available && requests == 2, "Prerelease-only page must not stop stable search"); ++passed;
        r = paginated(full_page(true), 200, "[]", requests);
        check(r.status == d::UpdateStatus::no_release && requests == 2, "Complete prerelease-only list has no stable release"); ++passed;
        r = paginated(full_page(false), 200, full_page(false), requests);
        check(r.status == d::UpdateStatus::failed && requests == 10, "Full pages must stop at the explicit request bound"); ++passed;
        r = paginated(full_page(false, true), 200, full_page(false, true), requests);
        check(r.status == d::UpdateStatus::failed && requests == 5, "Aggregate metadata must remain bounded"); ++passed;
        canceled = false;
        r = paginated(full_page(false), 200, "[]", requests, &canceled);
        check(r.status == d::UpdateStatus::canceled && requests == 2, "Later-page cancellation must suppress partial success"); ++passed;
        source.gitee_api = "https://gitee.com.evil.test/api/v5/repos/fleetingxhx/SonyConnect/releases";
        check(run(d::UpdateChannel::gitee, 200, "[]").status == d::UpdateStatus::failed, "Lookalike API origins must be rejected"); ++passed;
        std::cout << passed << " offline update scenarios passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
      catch (const winrt::hresult_error &e) { std::cerr << winrt::to_string(e.message()) << '\n'; return 1; }
}
