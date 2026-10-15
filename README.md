# SonyConnect for Windows

Windows 版 Sony 耳机控制程序。当前主工程采用 C++20、Win32、Windows SDK 和 CMake；界面仍为功能验证界面，正式 UI 与动效按后续阶段推进。

2026-10-14，用户确认当前 C++ 测试版的所有功能实测正常，记录见 [用户实测验收](docs/native/用户实测验收-20261014.json)。本次为当前 WF 使用范围的用户反馈，WH 等其他型号继续独立验收，不据此推断兼容。

## 运行与构建

已验收测试程序：[SonyConnect.exe](artifacts/native/packages/20261014-132318-110/portable/SonyConnect.exe)。

安装 Visual Studio“使用 C++ 的桌面开发”、Windows SDK 和 CMake 后，在工作区根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Build-Native.ps1 -Configuration Release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Publish-Native.ps1
```

编译输出为 `build/native-ninja/Release/SonyConnect.exe`，便携包位于 `artifacts/native/packages/`。构建不依赖 C# 工程或参考项目 checkout。

Debug/Release 的协议、会话、桌面、界面四组回归均已通过。离线验证与用户实测分别记录，详见 [迁移与验证](docs/native/迁移与验证.md) 和 [状态刷新修复](docs/native/P2与状态刷新修复-20261014.md)。

## 目录

| 目录 | 内容 |
| --- | --- |
| `projects/cpp/` | C++ 源码、测试、资源、配置和原生脚本 |
| `research/reference-projects/` | 参考源码与原许可，独立 Git 已关闭 |
| `research/official-app/` | 官方 App 原始 APK |
| `research/reverse-analysis/` | 逆向输出、派生帧和分析证据 |
| `docs/` | 验收、协议记录、目录映射、设计资料和历史文档 |
| `scripts/` | C++ 构建与发布入口 |
| `artifacts/` | 程序、日志、缓存、验收证据和维护记录，忽略 Git |
| `build/` | 本地 CMake 构建输出，忽略 Git |

C# 项目已迁至私有仓库 [FLEETING-XHX/SonyConnect-CSharp](https://github.com/FLEETING-XHX/SonyConnect-CSharp)。可运行程序保存在该仓库的 [私有 Release](https://github.com/FLEETING-XHX/SonyConnect-CSharp/releases/tag/csharp-archive-20261014)，本工作区只保留 C++ 活动工程。目录与核对结果见 [目录与仓库分离](docs/目录与仓库分离.md)。

C++、参考源码、官方 App 和逆向资料继续保留本地。旧 C# Git 历史及历史验收产物保留，主仓库未改写历史。C# 开发阶段的原 README 保存于 [历史原文](docs/history/README-CSharp-before-native-20261014.md)，其中旧路径按当时布局理解。

## 文档与维护

从 [文档目录](docs/README.md) 查看测试、实现、许可、型号待测与维护说明。WH 取回后的待测事项见 [WH 清单](docs/native/WH设备取回后待测.md)。

本地开发分支先提交，再集成到 `dev`；`main` 和主仓库远端不随本次整理更新。参考项目、APK、逆向资料与用户诊断/EQ 备份不进入 C# 私有归档。
