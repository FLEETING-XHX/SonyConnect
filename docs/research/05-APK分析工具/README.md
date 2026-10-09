# 05 APK 分析工具

| 本地入口 | 内容 |
| --- | --- |
| [JADX 官方 README](JADX-README.md) | APK / DEX 分析功能、用法、依赖和不能完整反编译的边界 |
| [JADX 许可](JADX-LICENSE) | 与下载的官方说明一并保留 |
| [已有 Sony APK](../../../reference-apk/SoundConnect-13.2.2.apk) | 官方来源的 13.2.2 安装包 |
| [原下载清单](../../../download-manifest.json) | APK 来源、大小和 SHA-256 |
| [SonyHeadphonesClient iOS 提取指南](../../../references/SonyHeadphonesClient/tooling/ida/README.md) | 从 iOS ARM64 / J2ObjC 信息提取 IR 的上游流程 |
| [IR 目录与清单](../../../references/SonyHeadphonesClient/tooling/ida/generated/inventory.json) | 已生成的协议资料入口 |

这里只保存 JADX 文档与许可，没有下载或安装 JADX 可执行程序、Java、IDA，也没有反编译或运行 APK。JADX 文档取自 master 快照，以下载时间和 SHA-256 标识；不是锁定的 JADX 发布版。

## APK 专项分析记录

针对开源资料未覆盖或有冲突的单一功能，先确定官方入口，再追踪按钮动作、设置读取、命令构造、结果处理和通知。每条结论记录 APK 版本、类与方法、调用路径，区分 UI 字符串、蓝牙协议、Android 系统接口和云请求；最终用耳机响应与行为确认。

SonyHeadphonesClient 当前协议提取资料是 iOS 路线，不能当成项目 APK 已被分析的证据。上游指南中的自动生成说明与不确定项原样保留，未逐字段验证。协议研究中的输入文件、反编译输出和日志建议与应用产品代码分开管理。
