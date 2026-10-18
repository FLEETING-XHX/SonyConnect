# SonyConnect for Windows

Windows 版 Sony 耳机控制程序。v0.1（版本号 0.1.0）采用 C++20、Win32、Windows SDK 和 CMake，使用正式主界面、更多设置、个性化、高级设置与诊断页面。简易 UI 和旧全局热键已移除，已完成清理后的针对性回归；最终候选实机验收边界见 [发布说明](docs/releases/v0.1.0.md)。

发行版入口：[GitHub](https://github.com/FLEETING-XHX/SonyConnect/releases) · [Gitee](https://gitee.com/fleetingxhx/SonyConnect/releases)。下载 Windows x64 ZIP 后完整解压运行；请保留 config 与 notices。

2026-10-14，用户确认当前 C++ 测试版的所有功能实测正常，记录见 [用户实测验收](docs/native/用户实测验收-20261014.json)。本次为当前 WF 使用范围的用户反馈，WH 等其他型号继续独立验收，不据此推断兼容。

## 运行与构建

当前构建程序：[SonyConnect.exe](build/native-ninja/Release/SonyConnect.exe)。历史验收包继续保留在 artifacts 中，不能替代当前清理版本的验收。

安装 Visual Studio“使用 C++ 的桌面开发”、Windows SDK 和 CMake 后，在工作区根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Build-Native.ps1 -Configuration Release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Publish-Native.ps1
```

编译输出为 `build/native-ninja/Release/SonyConnect.exe`，便携包位于 `artifacts/native/packages/`。构建不依赖 C# 工程或参考项目 checkout。

历史验证与用户实测分别记录，详见 [迁移与验证](docs/native/迁移与验证.md) 和 [状态刷新修复](docs/native/P2与状态刷新修复-20261014.md)。本次依赖分离、清理范围及当前验证边界见 [旧界面与热键清理](docs/ui/旧界面与热键清理-20261018.md)。

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

开发分支先提交，再集成到 `dev` 和 `main`；发行标签使用可比较的三段版本号。参考项目、APK、逆向资料与用户诊断/EQ 备份不进入原生便携包。
