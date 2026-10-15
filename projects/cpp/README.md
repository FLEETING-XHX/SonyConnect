# SonyConnect C++ 主工程

`native/` 保存 C++20 协议、会话、桌面服务、Win32 界面和测试；`config/` 为此实现自己的配置快照。参考源码、APK 和逆向资料不参与构建。

从主工作区运行 `scripts/Build-Native.ps1` 或本目录 `scripts/Build-Native.ps1`，均保持输出到主工作区 `build/native-ninja/<Configuration>/`。根目录 `CMakeLists.txt` 是兼容构建入口，实际目标定义位于本目录。

也可直接以本目录为 CMake source，在新的 build 目录中配置，避免与根目录 source 的已有 CMake cache 混用。构建和真机验收边界见主工作区 `docs/native/迁移与验证.md`。

本工程当前本地保留，未为本次整理创建或推送远端仓库。既有 EXE／便携 ZIP 保留在主工作区 `artifacts/native/packages/`。
