# SonyConnect C++ 主工程

`native/` 保存 C++20 协议、会话、桌面服务、Win32 界面和测试；`config/` 为此实现自己的配置快照。参考源码、APK 和逆向资料不参与构建。

从主工作区运行 `scripts/Build-Native.ps1` 或本目录 `scripts/Build-Native.ps1`，均保持输出到主工作区 `build/native-ninja/<Configuration>/`。根目录 `CMakeLists.txt` 是兼容构建入口，实际目标定义位于本目录。

也可直接以本目录为 CMake source，在新的 build 目录中配置，避免与根目录 source 的已有 CMake cache 混用。构建和真机验收边界见主工作区 `docs/native/迁移与验证.md`。

## 当前依赖与入口

正式程序统一位于 `build/native-ninja/<Configuration>/SonyConnect.exe`。构建目标分为启动入口 `SonyConnect`、应用适配层 `sonyconnect_app`、正式界面 `sonyconnect_ui` 和既有会话／协议／桌面服务。后台消息窗口不创建任何简易页面或控件；设备操作直接接收正式界面的参数。

旧全局热键、键盘钩子和录入／保存 API 已删除。现有本机 `hotkeys.json` 不读取、不覆盖、不删除。协议和会话源文件未改动。

现有正式滑条、更新、诊断自检入口继续保留；CTest 的旧 `native_ui` 已由正式适配检查 `native_app_adapter` 替代，不再打开七页简易 UI。清理与 85% 缩放候选已完成 9 组针对性回归及适配自检，实机与发布验收仍未执行。详见主工作区 `docs/ui/针对性回归-20261018.md`，清理历史见 `docs/ui/旧界面与热键清理-20261018.md`。

## 历史实现记录（2026-10-15～16）

下列独立试用目录属于当时构建缓存，现已清理。相关截图、日志与配置证据已迁存；当前运行入口以本页上方为准，历史说明不代表当前仍保留简易 UI。

当前试用入口：`build/native-state-flow-r7/Release/SonyConnect.exe`。本轮统一提交同尺寸完整窗口帧，避免状态更新后屏幕仍保留旧画面；并在用户批准后复用现有读取指令，补读外部音源变化后的固定播放状态，恢复经确认的音源切换与长按资格。详细证据、回归及硬件边界见 `docs/ui/重绘提交与固定状态补读-20261016-r7.md`。从托盘退出旧程序后再启动 r7，避免继续操作旧版。

`FLEETING/ui-main-window` 接入按 Pen 制作的原生主窗口（默认 Wake 外观），使用系统 Direct2D / DirectWrite；更多按钮暂时保留原七页功能窗口。主窗口无 Logo、无最大化、不可拖动缩放，保留拖动位置及工作区不足时滚动。所选银白 ICO 已嵌入程序与托盘。

使用 `build/native-ninja/Release/SonyConnect.exe --preview-main connected` 查看不连接设备的示例，或 `--preview-main ambient --preview-dpi 192` 查看高 DPI 通透布局。详情与尚未完成的子窗口、硬件验证边界见主工作区 `docs/ui/主窗口UI第一层-20261015.md`。

主窗口反馈修复见 `docs/ui/主窗口修复-20261015.md`：工作区右下定位、底部锚定过渡、原生 WakeSlider/RubberSegment、Pen 同源图标与围合长按，来源未知时不冒充零连接。CTest 增加动效与真实 HWND 离线场景，仍不代替耳机硬件验收。

最新四控件平滑迁移见 `docs/ui/四控件迁移与动效修复-20261015.md`：取消分段和滑条弹性，增加平滑开关、连续滑条与数值滚轮；使用单画布双缓冲消除独立子控件移动残影，确认提示改为静态文本。右键“更多”可选择 Wake / Elastic，普通点击仍打开既有功能窗口。

后续视觉与音压修复见 `docs/ui/主窗口视觉与音压修复-20261015-r3.md`：音量Wake为32根、环境音为20根；修正压缩圆角，正常操作隐藏回读标签，仅失败显示Pen橙色提示；播放区显示来源设备，并让主窗口可见时也能轮询音压。

所有后续窗口开发须遵循 `docs/ui/状态更新与无闪烁验收要求.md`，包括更多设置、高级设置、诊断与个性化；每完成一层就检查状态更新、输入过程和失败回滚是否产生闪烁。

动态布局与整帧窗口呈现见 `docs/ui/动态布局与窗口过渡-20261015-r4.md`：按模块测量高度、同步布局和动效，通过透明窗口整帧提交位置/尺寸/内容；连接页面加入正文过渡并缓存文字布局。独立试用程序位于 `build/native-layout-r4/Release/`。

本工程当前本地保留，未为本次整理创建或推送远端仓库。既有 EXE／便携 ZIP 保留在主工作区 `artifacts/native/packages/`。

最新鼠标交互修复见 `docs/ui/鼠标捕获与音源交互修复-20261016-r6.md`：修复旧控件失焦/捕获变化误取消新按钮，以及音源行在默认按钮处理松开前释放捕获的问题；覆盖六方向声音模式切换、音源单击与长按，200ms冷却保持不变。独立程序：`build/native-mode-input-r6/Release/SonyConnect.exe`。

主窗口稳定性及最新 Wake 样式见 `docs/ui/主窗口稳定性与Wake样式-20261015-r5.md`：音量32根、环境音20根，改为小圆角矩形；声音模式采用200ms最小切换间隔并等待未结束的回读；透明文字使用灰度抗锯齿，合并无效绘制并防止绘制重入。独立试用程序：`build/native-stability-r5/Release/SonyConnect.exe`。硬件连续切换稳定性仍需实机复测。
