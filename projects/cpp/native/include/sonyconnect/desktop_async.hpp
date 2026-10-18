#pragma once
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <atomic>
#include <chrono>
#include <memory>

namespace sonyconnect::desktop::detail {
// The completion delegate keeps only a weak signal, never the backend or host.
// Late completion is safe and an ignored cancellation cannot retain the event.
template <class Start>
auto await_async(Start start, const std::atomic_bool *closing = nullptr,
                 std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    if (closing && closing->load())
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED));
    struct Signal {
        winrt::handle ready{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        Signal() { if (!ready) winrt::throw_last_error(); }
    };
    auto signal = std::make_shared<Signal>();
    auto operation = start();
    operation.Completed([weak = std::weak_ptr<Signal>(signal)](auto const &, auto) {
        if (const auto ready = weak.lock()) SetEvent(ready->ready.get());
    });
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const bool canceled = closing && closing->load();
        const auto now = std::chrono::steady_clock::now();
        if (canceled || now >= deadline) {
            try { operation.Cancel(); } catch (...) {}
            throw winrt::hresult_error(HRESULT_FROM_WIN32(canceled ? ERROR_CANCELLED : ERROR_TIMEOUT));
        }
        const DWORD waited = WaitForSingleObject(signal->ready.get(), 20);
        if (waited == WAIT_OBJECT_0) {
            if (closing && closing->load()) {
                try { operation.Cancel(); } catch (...) {}
                throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            }
            return operation.GetResults();
        }
        if (waited == WAIT_FAILED) winrt::throw_last_error();
    }
}
} // namespace sonyconnect::desktop::detail
