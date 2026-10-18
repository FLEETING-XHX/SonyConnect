#pragma once
#include "sonyconnect/session.hpp"
#include "sonyconnect/desktop.hpp"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <memory>

namespace sonyconnect::ui {
enum class MainAction {
    close,
    more,
    discover,
    select_device,
    bluetooth_settings,
    power_off,
    sound_mode,
    ambient_level,
    voice_focus,
    multipoint,
    connection_preference,
    source,
    fix_source,
    release_source,
    volume,
    previous,
    play_pause,
    next,
    releases,
    slider_style,
    cancel_edit
};
enum class MainSliderStyle { wake, elastic };
enum class TrayAction { none, open, more, personalization, noise, ambient, off, quit };
// A presentation row may come from MDR or a separately verified Windows endpoint.
// Only a nonnegative protocol_index is allowed to route an MDR source command.
struct MainSourceRow {
    std::wstring label, status;
    protocol::SourceCategory category{protocol::SourceCategory::Unknown};
    int protocol_index{-1};
    bool active{}, connected{}, can_switch{}, can_fix{}, windows_endpoint{};
};
struct MainModel {
    DeviceSnapshot device;
    desktop::LocalPlaybackSnapshot local;
    std::vector<DeviceDescriptor> discovered;
    std::wstring feedback, pressure;
    std::vector<MainSourceRow> source_rows;
    std::wstring source_count, source_summary, operation_status;
    std::uint64_t operation_elapsed_ms{}, last_operation_elapsed_ms{};
    std::optional<int> volume_target, ambient_target;
    MainSliderStyle slider_style{MainSliderStyle::wake};
    std::optional<MainSliderStyle> ambient_slider_style;
    bool busy{}, discovering{}, preview{}, feedback_error{};
};
// Bounded UI readback scheduling; the controller owns the actual I/O and guards.
class MainFixationReadback {
  public:
    struct Context { std::uint64_t link{}; std::string source; };
    std::optional<Context> next(const DeviceSnapshot &, std::uint64_t now_ms);
  private:
    Context context_;
    unsigned attempts_{};
    std::uint64_t due_{};
};
// Pure projection. endpoint_current must mean the selected headphone, session and
// recent Windows endpoint snapshot were checked by the adapter; it is never inferred.
void project_main_sources(MainModel &, bool endpoint_current = false);
std::wstring main_media_source(const MainModel &);
// Presentation adapter only. Device/network/system work stays in the existing Window worker.
class MainWindow {
  public:
    using Dispatch = std::function<void(MainAction, int, bool, std::uint64_t)>;
    explicit MainWindow(Dispatch);
    ~MainWindow();
    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;
    void create(HINSTANCE, UINT preview_dpi = 0);
    HWND handle() const;
    void update(MainModel);
    void show(bool activate = true);
    void hide();
    TrayAction tray_menu(HWND owner);
    bool pre_translate(MSG &);
    void capture(const std::filesystem::path &);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Isolated visual fixture: never constructs a controller, hook, or network service.
MainModel main_preview(const std::wstring &state);
} // namespace sonyconnect::ui
