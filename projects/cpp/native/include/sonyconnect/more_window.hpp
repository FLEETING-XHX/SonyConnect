#pragma once
#include "sonyconnect/main_window.hpp"
#include <array>

namespace sonyconnect::ui {
enum class MoreAction {
    equalizer_preset,
    equalizer_band,
    clear_bass,
    dsee,
    touch_preset,
    cycle,
    guidance,
    prompt_volume,
    speak_to_chat,
    wear_pause,
    auto_power_off,
    startup,
    close_to_tray,
    personalization,
    advanced,
    diagnostics,
    releases,
    cancel_edit,
    slider_appearance,
    safe_listening,
    voice_assistant,
    restore_linked,
    custom_key,
    custom_action,
    custom_function,
    diagnostic_limits,
    diagnostic_export,
    diagnostic_replay,
    update_channel,
    check_update,
    release_page,
    licenses
};
enum class SettingsPage { more, personalization, advanced, diagnostics, replay };
struct MoreChoice {
    int value{};
    std::wstring label;
};
struct MoreControlPresentation {
    bool physical_nc_button{}, no_touch_sensor{};
    std::wstring cycle_label{L"触摸时切换的声音模式"};
    std::wstring dsee_label{L"DSEE"};
};
MoreControlPresentation more_control_presentation(const DeviceSnapshot &);
// Presentation values only. Device commands remain in the existing adapter.
struct MoreModel {
    DeviceSnapshot device;
    std::array<std::optional<int>, 6> equalizer_targets;
    std::optional<int> prompt_target;
    std::vector<MoreChoice> auto_power_choices;
    std::array<std::vector<MoreChoice>, 2> touch_choices;
    std::array<std::wstring, 2> touch_names;
    std::array<std::wstring, 2> touch_labels{L"左耳", L"右耳"};
    std::array<int, 2> touch_indices{-1, -1};
    std::wstring feedback;
    bool update_checking{};
    bool update_canceling{};
    int update_channel{};
    std::wstring update_message{L"尚未检查更新"};
    bool update_error{};
    unsigned log_mib{2}, log_runs{10};
    bool diagnostics_available{}, diagnostic_busy{}, diagnostic_error{};
    std::wstring diagnostic_status, diagnostic_feedback, replay_name;
    std::shared_ptr<const std::vector<desktop::DiagnosticReplayEntry>> replay_entries;
    MainSliderStyle slider_style{MainSliderStyle::wake};
    std::optional<MainSliderStyle> eq_slider_style, bass_slider_style;
    std::array<MainSliderStyle, 4> slider_styles{};
    std::vector<MoreChoice> assistant_choices;
    std::wstring assistant_name;
    bool linked_restore_available{};
    std::array<std::vector<MoreChoice>, 3> custom_choices;
    std::array<int, 3> custom_values{-1, -1, -1};
    int custom_context{-1};
    bool busy{}, offline{}, preview{}, feedback_error{}, startup{}, close_to_tray{};
};
class MoreWindow {
  public:
    using Dispatch = std::function<void(MoreAction, int, int, bool)>;
    explicit MoreWindow(Dispatch, SettingsPage = SettingsPage::more);
    ~MoreWindow();
    MoreWindow(const MoreWindow &) = delete;
    MoreWindow &operator=(const MoreWindow &) = delete;
    void create(HINSTANCE, HWND owner, UINT preview_dpi = 0);
    HWND handle() const;
    void update(MoreModel);
    void show();
    void hide();
    bool pre_translate(MSG &);
    void capture(const std::filesystem::path &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
MoreModel more_preview(MainSliderStyle = MainSliderStyle::wake);
struct ReplayText { std::wstring time, title, detail; bool error{}; };
ReplayText replay_text(const desktop::DiagnosticReplayEntry &);
} // namespace sonyconnect::ui
