# SonyConnect for Windows

Windows 版 Sony 耳机控制程序。当前 `C++` 分支以 C++20／Win32／CMake 原生工程为开发入口；原 C#／WPF 工程保留作为回归与回退基线。仍使用功能验证界面，正式设计 UI 和动效按后续阶段接入。

## 本地目录整理（2026-10-14）

当前工程分为 `projects/cpp/` 与 `projects/csharp/`；参考项目、官方 APK 和逆向输出分别保存在 `research/reference-projects/`、`research/official-app/` 与 `research/reverse-analysis/`。

参考项目的 10 处 Git 元数据已备份移出；源码与许可保留，主项目 Git 不变。C# 已整理成可独立建仓的目录，本轮未创建或推送私有仓库。路径映射、备份与后续仓库范围见 [目录与仓库分离](docs/目录与仓库分离.md)。下方旧 C# 记录中的原路径是历史位置，当前路径按映射对应。

## C++ 迁移（2026-10-14）

打开仓库目录或 `CMakeLists.txt`。本机已有 Visual Studio 的“使用 C++ 的桌面开发”、Windows SDK 和 CMake 时，运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Build-Native.ps1 -Configuration Release
```

入口：`build/native-ninja/Release/SonyConnect.exe`。这条构建路径不调用 dotnet，也不依赖 `research/reference-projects/` 中的上游工程。协议、蓝牙会话、设备设置、热键、本机媒体、更新与诊断已迁入 `projects/cpp/native/`；握手依据、验证记录及迁移边界见 [C++ 迁移说明](docs/native/迁移与验证.md)。原 C# 的实机结论不能自动当作 C++ 验证结果，C++ 真机验收仍待完成。

下方保留此前 C# 基线的开发与测试记录。

## 本轮开发（2026-10-14）

更新检测提供“GitHub 更新／Gitee 更新”两个选项；Gitee 已配置为用户提供的 [fleetingxhx/SonyConnect](https://gitee.com/fleetingxhx/SonyConnect)，版本清单使用 main 根目录 update.json。清单模板见 `config/gitee-update-manifest.example.json`；需实际发布安装包并填写版本后上传。本环境匿名访问返回 403／404，尚未验证线上检测／下载。GitHub 公开仍为后续计划，本轮没有修改远端可见性或发布文件。

`FLEETING/sc-20-22-hotkeys-update-baseline` 已接入自定义热键与用户本次授权的软件更新检测。热键按一个识别键加一个自由键保存，支持左右 Ctrl／Alt／Shift、Caps Lock、Tab 与 Home／End／PgUp／PgDn 等；默认右 Ctrl + F1／F2／F3。测试版与真实键盘待验收步骤见 [SC-20～22 验收](docs/测试与验收.md#sc20-22)。正式 UI 尚未准入，本地 dev 未合并本轮改动。

本轮累计验证入口：`powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Test-SC20-22.ps1 -Offline`。下面为已集成 dev 的上一轮基线。

## 当前状态（2026-10-13）

- 本地 `dev` 已快进合并到 `a75d581`，包含 SC-1～18 的共用实现与 WF 基线；未推送远程，未发布正式版本。
- WF-1000XM5 的已实现功能已有设备记录或用户实测反馈。累计离线回归 230 项、Debug／Release、单 EXE 和界面检查通过。
- WH-1000XM5、LinkBuds S（WF-LS900N）、WH-CH720N 分别由 SC-30～32 独立适配／验收，不能从 WF 结果推断兼容。
- 正式 UI、热键、更新检测、最终综合回归和发布仍待完成；音压没有可信查询／单位，保留受限结论。

## 运行与验证

本机入口：[当前测试版](artifacts/sc-3/01-当前测试版.lnk)、[回退版本](artifacts/sc-3/02-回退版本.lnk)、[SC-12～15 冻结版](artifacts/sc-3/03-SC12-15实测冻结版.lnk)。先从托盘退出其他版本和手机 Sony 控制 App；两台设备的音频连接可并存，控制通道须交替使用。路径见 [程序入口说明](artifacts/sc-3/00-程序入口说明.md)。

Rider 打开 `SonyConnect.sln`，选择 `SonyConnect.App`。SDK 版本见 `global.json`，构建 `dotnet build SonyConnect.sln`。完整离线验证入口：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/Test-SC16-18.ps1 -Offline
```

单文件发布无需预装 .NET，但运行仍有解包缓存和用户设置。首次恢复依赖可能需联网；现有离线依赖保留在 `artifacts/`。

## 文档入口

- [文档目录](docs/README.md)：需求、架构、研究、维护及设计资源。
- [测试与验收](docs/测试与验收.md)：最新结论、操作边界和历史实测记录；原始 JSON／日志／截图保留。
- [开发与协议记录](docs/开发与协议记录.md)：实现、技术决策、协议修复与历史断点。
- [任务与验收状态](docs/任务与验收状态.md)：阶段任务及独立型号 Issue。
- [范围与许可](docs/范围与许可.md)：功能／型号矩阵和许可边界。
- [项目维护](docs/项目维护.md)：分支、产物保留与清理规则。
- [研究资料](docs/research/README.md)：固定上游、APK 和反编译位置。

## 项目目录

| 目录 | 用途 |
| --- | --- |
| `projects/cpp/` | C++ 主工程、配置和原生脚本 |
| `projects/csharp/` | C#／WPF 回归工程，准备后续私有仓库 |
| `research/reference-projects/` | 已关闭独立 Git 的参考源码快照 |
| `research/official-app/` | 官方 APK 原件 |
| `research/reverse-analysis/` | 静态反编译、派生帧和逆向证据 |
| `docs/` | 需求、验证记录、索引与设计资料 |
| `scripts/` | 兼容构建／发布／回归入口 |
| `artifacts/`、`build/` | 程序、报告、缓存、备份与构建输出，忽略 Git |

## 来源与同步

上游版本与下载校验见 [download-manifest.json](download-manifest.json)、[研究资料清单](docs/research/资料清单.json)。参考仓库、APK、原始许可和反编译产物保留；Gadgetbridge 仅供交叉研究，不能将其 AGPL 代码直接纳入产品。

仓库：[FLEETING-XHX/SonyConnect](https://github.com/FLEETING-XHX/SonyConnect)。开发分支 → `dev` 集成 → `main` 发布；本地提交与远程推送分别授权。整理文档不等于已发布或完成全型号兼容。
