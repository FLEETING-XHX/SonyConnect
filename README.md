# SonyConnect for Windows

## 开发方式（2026-10-09 用户确认）

采用模块化架构，先用简易 UI 实现并验证主要功能，再切换现有设计 UI 和动效。两种 UI 复用设备状态、业务操作与协议模块。模块边界、开发阶段及切换验收条件见 [模块化架构与分阶段开发](docs/模块化架构与分阶段开发.md)。
## 目录

- references/SonyHeadphonesClient：mos9527/SonyHeadphonesClient，耳机协议与跨平台参考实现。
- references/sony-tray：abhiinhii/sony-tray，C# / WPF Windows 托盘控制器，适合参考托盘与蓝牙会话。
- references/sony-device-center：marconvcm/sony-device-center，C++ / Qt 设置界面、设备状态与高级功能参考。
- reference-apk/SoundConnect-13.2.2.apk：Sony 中国官网提供的 Android 安装包。
- download-manifest.json：下载来源、各仓库提交、分支、子模块和 APK SHA-256。

三个仓库以浅克隆方式下载，保留 .git 和上游地址，已初始化已声明的子模块。完整历史可在相应目录运行 git fetch --unshallow 获取。构建时仍可能需要联网获取 NuGet、CMake 等声明的依赖；本次未构建或运行任何程序。

## 已确认的产品要求（2026-10-08）

1. Windows 桌面程序，面向 WF-1000XM5 与 WH-1000XM5。
2. 中文优先，交互参考 GHelper：常驻托盘，点击图标在所在显示器托盘附近弹出控制面板。
3. 功能目标：按官方 Sound Connect APK 的全部功能逐项盘点，尽可能实现 Windows 对应功能。不能以做出同名控件代替真实耳机控制。
4. 核心交互包含耳机选择、连接状态、电量、降噪/环境声/关闭、环境声强度、EQ 与 Clear Bass，以及设备支持的其他功能开关。
5. 发布交付仅一个自包含 EXE，双击运行，无需安装程序或预装 .NET；依赖及资源嵌入。运行时解包、缓存与用户设置存储需在实现时明确，单文件分发不等于运行时零文件写入。
6. 当前先由用户进行 UI 设计，本次仅准备参考源码和 APK，尚未开始应用实现。

## 后续功能核对方式

对 APK 每项功能记录：入口、交互、当前设置读取、修改命令、两种耳机支持情况、依赖、验证状态。

耳机内部功能需要蓝牙协议与实机验证；手机传感器/定位、Sony 账号与云服务、第三方服务、系统编解码器和固件更新需单独评估对应实现条件。目标是功能覆盖，当前不能宣称 APK 全功能已在 Windows 可用。

## 来源与许可

- https://github.com/mos9527/SonyHeadphonesClient
- https://github.com/abhiinhii/sony-tray
- https://github.com/marconvcm/sony-device-center
- Sony 官方下载页：https://www.sonystyle.com.cn/minisite/cross/appcenter/pa/sound_connect/index.html
- APK 直链：https://www.sonystyle.com.cn/minisite/cross/app/download/pa/sound_connect_v13.2.2_1.apk

三个仓库主项目的 MIT 许可与原作者版权文件保留在各自目录，发布时还需核对依赖许可。官方 APK 作为本地分析和 UI 参考资料；本次未执行、安装、反编译或验证其签名证书。

## SonyConnect 补充资料（2026-10-09）

已保存四个补充仓库、固定播放 PR 讨论与补丁、官方抓包与 Windows 开发资料，按用途分类。入口见 [补充资料总目录](docs/research/README.md)，版本、来源与校验记录见 [资料清单](docs/research/资料清单.json)。原有参考仓库与 APK 保留；Gadgetbridge 仅稀疏检出 Sony 耳机相关资料。

## 远程仓库与同步范围

- 仓库：[FLEETING-XHX/SonyConnect](https://github.com/FLEETING-XHX/SonyConnect)，当前为私有仓库，默认分支 main。
- 软件发布页：[Releases](https://github.com/FLEETING-XHX/SonyConnect/releases)。当前尚未发布可运行版本。
- 本地项目目录：SoundConnect；软件和 GitHub 仓库名称：SonyConnect。
- Git 同步文档、UI 截图、资料索引和来源清单。references/ 内的独立上游仓库、reference-apk/ 内的官方 APK、构建产物与缓存沿用 .gitignore，保留本地，不纳入本项目提交。
- 参考资料版本与获取地址见 download-manifest.json 和 docs/research/资料清单.json；使用另一台电脑时需按这些清单准备本地参考资料。本地源码入口在未恢复 references/ 时不可打开。
- 更新检测的目标配置见 config/update-source.json。检测逻辑、版本文字状态与默认浏览器跳转仍待应用开发；私有仓库的 API 查询需要认证，发布前应明确公开发布渠道或认证方案，不能把个人访问令牌嵌入软件。

首次同步后，常规更新流程为 git add、git commit、git push；main 已连接 origin/main。
