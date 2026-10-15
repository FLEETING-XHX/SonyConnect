# 研究资料与分析入口

## 当前分析位置（2026-10-14）

参考源码现位于 `research/reference-projects/`，独立 Git 已关闭；固定提交、来源与备份见根目录 `research/reference-projects-state.json`。历史下载路径保留作原始记录。

- 原 APK：`research/official-app/SoundConnect-13.2.2.apk`。
- 已完成的静态反编译／分析：`research/reverse-analysis/rea-sony-static-20261013/sources/`；SC-14 专项：`research/reverse-analysis/rea-sc14-20261013/sources/`。这些目录保留，不是普通构建缓存。
- 固定上游与许可：`download-manifest.json`、本目录 `资料清单.json`、[范围与许可](../范围与许可.md)。跨型号结论见 [Sony TWS 协议共用与兼容边界](Sony-TWS协议共用与兼容边界-20261013.md)。

下方“未反编译”等表述是下载资料时的历史状态，不能覆盖当前分析入口；反编译证据不等于实机兼容性。

# SonyConnect 补充资料总目录

归档日期：2026-10-09。这里整理开发 SonyConnect 值得保留的协议、源码、设备支持与调试资料。入口均指向本地文件；网页原始来源、下载时间、大小与 SHA-256，仓库分支与提交见 [资料清单](资料清单.json)。

## 分类入口

| 分类 | 内容 |
| --- | --- |
| [01 协议文档](#doc-c675d0f8be14) | Tandem / MDR 封包、旧协议逆向记录、协议版本和生成头文件入口 |
| [02 设备支持](#doc-5acb24ce6735) | WF / WH-1000XM5 上游支持表、Gadgetbridge 支持页面和型号能力源码 |
| [03 双设备与固定播放](#doc-520583633485) | PR #57 离线讨论、补丁、字段含义及拒绝场景 |
| [04 抓包与调试](#doc-6a0c314fced6) | Android HCI 日志、离线回放、模拟传输、问题定位记录模板 |
| [05 APK 分析工具](#doc-2b9589dabf4e) | JADX 官方说明与许可、已有 APK、上游 iOS 协议提取流程 |
| [06 Windows 开发](#doc-92709104ddd5) | WPF、RFCOMM、系统音量、媒体控制、快捷键、便携发布、性能和更新检测 |

## 新增仓库的位置

| 仓库 | 本地入口 | 定位 |
| --- | --- | --- |
| AndreasOlofsson/mdr-protocol | [README](../../research/reference-projects/supplemental/01-protocol-docs/mdr-protocol/README.md) | 旧设备封包基础与逆向方法；文档不完整 |
| ohm-app/sony-headphones-bluetooth-documentation | [文档首页](../../research/reference-projects/supplemental/01-protocol-docs/sony-headphones-bluetooth-documentation/docs/index.md) | 研究中的独立协议笔记；有未知字段 |
| AmitRajput-Dev/SonyBridge | [README](../../research/reference-projects/supplemental/02-desktop-implementations/SonyBridge/README.md) | 另一套桌面实现；保留 imgui 子模块 |
| Freeyourgadget/Gadgetbridge | [README](../../research/reference-projects/supplemental/03-android-implementation/Gadgetbridge/README.md) | Sony 耳机相关 Android 协议、型号能力和测试 |

Gadgetbridge 使用浅克隆、部分克隆和稀疏检出，保存 Sony 耳机的设备层、协议层、测试及设置 XML；不是可直接构建的完整 Android 工作目录。其它新增仓库为浅克隆，保留 Git 上游与提交信息。原来的三个参考仓库、APK 和 download-manifest.json 没有替换。

## 使用顺序与验证边界

先看 [协议研究路线](../协议研究路线与资料索引.md)，再按功能查本目录。协议以 SonyHeadphonesClient 定义与测试为主，sony-tray 提供 C# Windows 会话参考，Gadgetbridge 用于交叉核对，APK 用于缺项专项分析；派生项目共享的代码不能当作独立实机验证。

这次只归档资料并检查版本、文件与网页内容；没有运行参考软件、构建应用、安装分析工具或反编译 APK，也没有在用户耳机上验证命令。上游的“支持”与 SonyConnect 的“已实现”分别记录。

保存的 HTML 包含正文，但 CSS、图片、脚本可能仍指向外站；属于正文离线快照，不是完整网站镜像。GitHub PR API 本次返回 403 限流，讨论改为保存公开 HTML 页面，改动保存为 patch；失败记录保留在清单里。

## 维护规则

- references/ 与 reference-apk/ 延续项目现有 .gitignore；本地资料已保存，但不会自动进入项目自身 Git 提交。
- docs/research/ 中的索引、网页与清单可以随项目 Git 版本管理。各上游仓库自己的 .git 保留，用 commit 对照资料。
- 原文与源码保持上游内容。后续研究结论写在项目说明中，并注明型号、固件、来源和验证级别。
- 更新资料时记录新 commit，不用“最新版本”代替可追溯的版本。网页快照按日期追加或明确替换。
- 上游许可保留在各仓库。Gadgetbridge Sony 源码标明 AGPL-3.0-or-later，ohm 文档为 CC0，SonyBridge 为 MIT；mdr-protocol 当前 checkout 没有独立 LICENSE 文件，直接复制到产品前需核对授权。


## 分类资料（原文归档）

## 合并章节

- [docs/research/01-协议文档/README.md](#doc-c675d0f8be14)
- [docs/research/02-设备支持/README.md](#doc-5acb24ce6735)
- [docs/research/03-双设备与固定播放/README.md](#doc-520583633485)
- [docs/research/04-抓包与调试/README.md](#doc-6a0c314fced6)
- [docs/research/05-APK分析工具/README.md](#doc-2b9589dabf4e)
- [docs/research/06-Windows开发/README.md](#doc-92709104ddd5)

<a id="doc-c675d0f8be14"></a>

## docs/research/01-协议文档/README.md

<details>
<summary>查看完整历史记录／技术内容</summary>

# 01 协议文档

## 先读封包，再查具体功能

| 本地资料 | 用途与范围 |
| --- | --- |
| [mdr-protocol README](../../research/reference-projects/supplemental/01-protocol-docs/mdr-protocol/README.md) | Tandem 帧、转义、序号、ACK 与校验；来自旧设备研究 |
| [MDR 消息分类](../../research/reference-projects/supplemental/01-protocol-docs/mdr-protocol/MDR_packets.md) | MDR 命令分类；不是 XM5 的完整规格 |
| [逆向方法](../../research/reference-projects/supplemental/01-protocol-docs/mdr-protocol/reverse_engineering.md) | 如何对照官方 App 操作与蓝牙数据 |
| [ohm 协议笔记](../../research/reference-projects/supplemental/01-protocol-docs/sony-headphones-bluetooth-documentation/docs/Protocol/Specification.md) | RFCOMM 数据分析；未知项原样保留 |
| [SonyHeadphonesClient V2/T2 协议头](../../research/reference-projects/SonyHeadphonesClient/libmdr/include/mdr/ProtocolV2T2.hpp) | 与较新耳机功能相关的定义入口 |
| [公开 C 接口](../../research/reference-projects/SonyHeadphonesClient/libmdr/include/mdr-c/Headphones.h) | 功能、状态、能力和结果读取接口 |
| [协议生成工具说明](../../research/reference-projects/SonyHeadphonesClient/tooling/README.md) | 生成协议、序列化与校验代码的流程 |
| [上游贡献规则](../../research/reference-projects/SonyHeadphonesClient/.github/CONTRIBUTING.md) | 协议头应由工具生成，避免手改导致不一致 |

## 读源码时分清层次

Windows Bluetooth / RFCOMM 负责连接和字节流；Tandem 负责封包与 ACK；MDR 负责耳机命令。抓包中的 RFCOMM 外层字段不应重复拼进应用的 MDR 数据包。旧设备服务 UUID 也不能直接套用到 XM5，应对照设备协议代际、服务发现与现有 Windows 实现。

当前 SonyHeadphonesClient 的 IR 来源是官方 iOS 二进制，不是项目保存的 Android APK；具体说明见 [05 APK 分析工具](README.md#doc-2b9589dabf4e)。

## 功能研究记录模板

| 字段 | 待填写 |
| --- | --- |
| 功能 / 型号 / 固件 | WF 与 WH 分开记录 |
| 来源 | 仓库 commit、文件与符号；或 APK 版本 / 类 / 方法；或抓包编号 |
| 协议版本与能力条件 | 连接握手、功能 capability、有效范围 |
| 请求 / 响应 / 通知 | 命令、字段、字节顺序、ACK、回读与失败原因 |
| 操作时序 | 发送前状态、确认、外部设置变更、重连后的刷新 |
| 验证级别 | 有定义 / 有实现 / 上游实机报告 / 本机实测 / 回归测试 |
| 待解决问题 | 冲突字段、未知枚举、未覆盖的固件和场景 |

协议字段定义、代码实现和实机效果分别确认；资料归档不代表全部功能已可用。

</details>

<a id="doc-5acb24ce6735"></a>

## docs/research/02-设备支持/README.md

<details>
<summary>查看完整历史记录／技术内容</summary>

# 02 设备支持

| 本地入口 | 核对内容 |
| --- | --- |
| [SonyHeadphonesClient WF-1000XM5 支持表](../../research/reference-projects/SonyHeadphonesClient/docs/device-support/WF-1000XM5.md) | WF 功能列表、上游支持状态 |
| [SonyHeadphonesClient WH-1000XM5 支持表](../../research/reference-projects/SonyHeadphonesClient/docs/device-support/WH-1000XM5.md) | WH 功能支持，与 WF 分开判断 |
| [sony-device-center 设备矩阵](../../research/reference-projects/sony-device-center/docs/device-matrix.md) | 已测试 / 未测试范围 |
| [Gadgetbridge Sony 支持页面](02-设备支持/Gadgetbridge-Sony支持情况.html) | 型号、缺项及相关 issue 的网页快照 |
| [Gadgetbridge WF-1000XM5 型号能力源码](../../research/reference-projects/supplemental/03-android-implementation/Gadgetbridge/app/src/main/java/nodomain/freeyourgadget/gadgetbridge/devices/sony/headphones/coordinators/SonyWF1000XM5Coordinator.java) | 默认能力、左右耳与盒子电量、实验性状态 |
| [Gadgetbridge WH-1000XM5 型号能力源码](../../research/reference-projects/supplemental/03-android-implementation/Gadgetbridge/app/src/main/java/nodomain/freeyourgadget/gadgetbridge/devices/sony/headphones/coordinators/SonyWH1000XM5Coordinator.java) | WH 默认能力 |
| [SonyBridge 支持范围](../../research/reference-projects/supplemental/02-desktop-implementations/SonyBridge/README.md) | 另一客户端的实测与预期支持区分 |

已读取 Gadgetbridge 的 WF-1000XM5 coordinator：isExperimental() 返回 true，源码注释说明环境声控制未完全工作、耳机音量控制可能有问题。默认 capability 列表不是在本机动态查询得到的结果，不能据此把高级功能标成用户耳机已验证。

SonyConnect 应以连接时读到的协议版本与能力决定控件状态，并记录本机实测。耳机盒电量可能不是实时值，应结合设备实际返回与更新时间呈现。用户已取消的“通话过程中捕获语音”不因参考源码存在该功能而重新加入产品范围。

</details>

<a id="doc-520583633485"></a>

## docs/research/03-双设备与固定播放/README.md

<details>
<summary>查看完整历史记录／技术内容</summary>

# 03 双设备与固定播放

原始来源：[SonyHeadphonesClient PR #57](https://github.com/mos9527/SonyHeadphonesClient/pull/57)。归档内容：

- [讨论与实机报告 HTML](03-双设备与固定播放/PR57-讨论与实机记录.html)
- [提交补丁 patch](03-双设备与固定播放/PR57-固定播放.patch)
- [现有 C 接口](../../research/reference-projects/SonyHeadphonesClient/libmdr/include/mdr-c/Headphones.h)

作者 jkolo 报告使用 WF-1000XM5、固件 6.1.0 对照官方 App 验证固定与解除行为。归档页面包含该型号、固件与 SOURCE_SWITCH_CONTROL 讨论。此项是上游报告，仍需用户耳机验证。

## 实现时必须保留的细节

1. SOURCE_SWITCH_CONTROL 表示允许自动切换：1 = 允许，0 = 固定。UI 的“固定”布尔值需要转换。
2. 固定作用于当前播放来源；要固定另一个设备，应先切换来源并确认，再提交固定命令。
3. 通话或语音助手占用时可能拒绝固定；读取结果和失败原因，不能只更新图标。
4. PR 增加了未固定状态和通话拒绝状态的接收样本，未附成功固定状态抓包。不能将这些样本写成覆盖成功固定的自动测试。
5. 本地 API 的结果读取与设置操作、设备列表变化关联；移植时核对操作生命周期。

产品交互仍以 [功能与交互说明](../功能与交互说明.md) 为准：单击选音源，长按逆时针描边完成后请求固定；等待耳机确认后显示声音带锁图标；右键带锁设备解除固定。提前松开、移出或取消时不发送固定；请求失败时回退到真实设备状态。

## 建议实机用例

| 场景 | 观察 |
| --- | --- |
| A 正在播放，选择 B 后固定 | 活动来源确认后再固定，B 图标与设备实际状态一致 |
| 固定 A，B 开始播放 | A 是否仍是耳机来源；两个播放应用的行为分别记录 |
| 右键解除 | 回读允许切换，图标同步 |
| 正在通话 / 语音助手占用 | 拒绝结果与用户反馈 |
| 长按中断开 / 已固定设备断开 | 取消进度与命令，刷新设备状态 |
| 官方 App 在另一端修改固定 | 通知同步；没有通知时按实际可用的回读方式刷新 |

这些是后续验收用例，本轮没有执行。GitHub API 因 403 限流未保存 JSON 数据，已用公开 PR 页面与 patch 保存主要证据。

</details>

<a id="doc-6a0c314fced6"></a>

## docs/research/04-抓包与调试/README.md

<details>
<summary>查看完整历史记录／技术内容</summary>

# 04 抓包与调试

| 本地入口 | 用途 |
| --- | --- |
| [Android 官方 HCI 日志指南](04-抓包与调试/Android-蓝牙HCI日志.html) | 官方 App 操作对应的蓝牙交换；日志导出按手机系统确认 |
| [SonyHeadphonesClient 记录与回放指南](../../research/reference-projects/SonyHeadphonesClient/tests/README.md) | record、replay、协议调试器与回归测试 |
| [WF-1000XM5 固件 6.1.0 样本目录](../../research/reference-projects/SonyHeadphonesClient/tests/WF-1000XM5-6.1.0) | 已在原仓库保留的接收数据包 |
| [抓包数据清理工具源码](../../research/reference-projects/SonyHeadphonesClient/tooling/scrub-capture.py) | 保持长度与校验规则的隐私字段处理参考 |
| [sony-tray 模拟传输](../../research/reference-projects/sony-tray/tests/SonyTray.Tests/FakeTransport.cs) | 离线模拟连接、发送与接收 |
| [sony-device-center 架构](../../research/reference-projects/sony-device-center/docs/architecture-current.md) | 协议、设备状态和界面分层 |
| [IPC 与生命周期](../../research/reference-projects/sony-device-center/docs/ipc-and-lifecycle.md) | 连接生命周期、进程与事件处理 |
| [技术债记录](../../research/reference-projects/sony-device-center/docs/technical-debt.md) | 未完成范围和故障处理线索 |

## SonyConnect 后续诊断设计建议

建议开发初期提供可开关的诊断日志，按连接、协议、设备状态和 UI 分层记录。一次命令带关联编号，串起请求、发送、ACK、响应、超时与回读，避免只留下“操作失败”。日志同时记录应用版本、Windows 版本、蓝牙适配器、型号、固件和协议代际。

UI 动画完成不代表耳机已执行；界面先表示请求中，再根据确认 / 回读进入成功状态。外部触摸、官方 App 修改、音源变化和重连也要经过同一状态更新链。

## 定位问题的顺序

1. 先确认 Windows 音频连接与 Sony 控制链路各自状态。能听到音乐不代表控制服务已连通。
2. 检查服务发现、连接、握手与能力查询。
3. 对照目标命令的 TX / RX 和超时；区别没有响应、格式不兼容、能力不支持和设备主动拒绝。
4. 在相同型号 / 固件 / 音源状态下对照官方 App 的一次操作；每次只改一项，记录操作时间。
5. 把能复现问题的数据整理成离线回放或模拟传输用例，再验证修复后的实机行为。
6. 若状态正确而显示错误，再查 UI 绑定、事件顺序、动画取消与线程切换。

上游记录命令可能清空目标目录中已有包，执行前阅读原指南并使用新的抓包目录。共享问题记录时处理设备地址、名称与媒体标题。工具源码和包样本本轮只保存，没有执行。

## Bug 记录模板

- 应用版本 / commit：
- 型号 / 固件 / Windows / 适配器：
- 两台设备、活动音源、声音模式、通话状态：
- 重现步骤 / 预期行为 / 实际行为：
- 发生时间 / 命令关联编号 / 日志或抓包路径：
- 官方 App 同场景结果：
- 最小离线复现与修复后结果：

</details>

<a id="doc-2b9589dabf4e"></a>

## docs/research/05-APK分析工具/README.md

<details>
<summary>查看完整历史记录／技术内容</summary>

# 05 APK 分析工具

| 本地入口 | 内容 |
| --- | --- |
| [JADX 官方 README](05-APK分析工具/JADX-README.md) | APK / DEX 分析功能、用法、依赖和不能完整反编译的边界 |
| [JADX 许可](05-APK分析工具/JADX-LICENSE) | 与下载的官方说明一并保留 |
| [已有 Sony APK](../../research/official-app/SoundConnect-13.2.2.apk) | 官方来源的 13.2.2 安装包 |
| [原下载清单](../../download-manifest.json) | APK 来源、大小和 SHA-256 |
| [SonyHeadphonesClient iOS 提取指南](../../research/reference-projects/SonyHeadphonesClient/tooling/ida/README.md) | 从 iOS ARM64 / J2ObjC 信息提取 IR 的上游流程 |
| [IR 目录与清单](../../research/reference-projects/SonyHeadphonesClient/tooling/ida/generated/inventory.json) | 已生成的协议资料入口 |

这里只保存 JADX 文档与许可，没有下载或安装 JADX 可执行程序、Java、IDA，也没有反编译或运行 APK。JADX 文档取自 master 快照，以下载时间和 SHA-256 标识；不是锁定的 JADX 发布版。

## APK 专项分析记录

针对开源资料未覆盖或有冲突的单一功能，先确定官方入口，再追踪按钮动作、设置读取、命令构造、结果处理和通知。每条结论记录 APK 版本、类与方法、调用路径，区分 UI 字符串、蓝牙协议、Android 系统接口和云请求；最终用耳机响应与行为确认。

SonyHeadphonesClient 当前协议提取资料是 iOS 路线，不能当成项目 APK 已被分析的证据。上游指南中的自动生成说明与不确定项原样保留，未逐字段验证。协议研究中的输入文件、反编译输出和日志建议与应用产品代码分开管理。

</details>

<a id="doc-92709104ddd5"></a>

## docs/research/06-Windows开发/README.md

<details>
<summary>查看完整历史记录／技术内容</summary>

# 06 Windows 开发

以下文件为官方文档正文快照，来源和校验值见资料清单。

| 离线文档 | 与 SonyConnect 的对应关系 |
| --- | --- |
| [WPF 概览](06-Windows开发/WPF-概览.html) | C# 桌面 UI 与控件基础 |
| [RFCOMM 接口](06-Windows开发/Windows-RFCOMM接口.html) | Windows 蓝牙控制连接 |
| [系统音量接口](06-Windows开发/Windows-系统音量接口.html) | IAudioEndpointVolume：系统音频端点音量与变化通知 |
| [媒体会话接口](06-Windows开发/Windows-媒体会话接口.html) | 播放 / 暂停、曲目或视频信息；取决于媒体应用提供的会话 |
| [全局快捷键](06-Windows开发/Windows-全局快捷键.html) | RegisterHotKey、快捷键注册与冲突处理 |
| [蓝牙设备类型](06-Windows开发/Windows-蓝牙设备类型.html) | Class of Device 与设备大类 |
| [NET 单文件发布](06-Windows开发/NET-单文件发布.html) | 单文件、自包含与运行时文件提取 |
| [NET 裁剪不兼容情况](06-Windows开发/NET-裁剪不兼容情况.html) | WPF 等裁剪限制，避免盲目启用 Trim |
| [Visual Studio 性能分析](06-Windows开发/VisualStudio-性能分析.html) | CPU、内存、分配与性能分析 |
| [NET 运行时计数器](06-Windows开发/NET-运行时计数器.html) | 运行时指标与 dotnet-counters 工具说明 |
| [GitHub Releases 接口](06-Windows开发/GitHub-Releases接口.html) | 后续检测更新与 Releases 跳转的接口资料 |

## 实现边界备忘

- Windows 看到的设备分类可辅助 PC / 手机图标；耳机返回的远端设备列表是否带类型需另外核对。Windows 本机 API 不保证能读取耳机连接的另一台手机的完整信息，未知设备保留通用图标与手动更正入口的可能性。
- 系统音量、耳机内部音量和当前活动音源分别建模。媒体控制与标题来自可用会话，遇到不支持的播放器提供中性占位，不能假定任何视频都可读取名称。
- 默认快捷键已改为 R-Ctrl+F1～F3，Reset 恢复右 Ctrl 组合。RegisterHotKey 的 MOD_CONTROL 不区分左右 Ctrl，指定右侧需要额外的按键识别路径；实现与冲突处理仍需验证。
- 耳机连接质量偏好不等于强制系统使用某个 codec；当前活动来源和两端能力一起决定协商结果。SBC / AAC / LDAC 的显示依靠实际可读取状态。Gadgetbridge 的 AudioCodec 枚举提供交叉核对线索，不硬编码“稳定优先总是 AAC”。
- 单 EXE、自包含和免安装的目标仍要测量发布体积、空闲内存、托盘状态与动画时的 CPU。WPF 的裁剪限制见官方文档；体积和占用本轮没有实测数值。
- React Bits 只作为动效参考；WPF 需实现相应的动画与交互，不能直接把 React 组件当 WPF 控件。动画只在交互时运行，后台与隐藏窗口停止无必要的更新。
- 本项目仓库已创建：https://github.com/FLEETING-XHX/SonyConnect，发布页为该仓库 /releases，目标配置见 ../../../config/update-source.json。仓库目前私有，匿名 API 查询不可视为已支持；检测逻辑、红色版本提示与点击跳转待应用开发。

本轮没有安装开发环境或执行上述工具；具体依赖与发布参数在开始实现时核对。

</details>
