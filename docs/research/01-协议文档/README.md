# 01 协议文档

## 先读封包，再查具体功能

| 本地资料 | 用途与范围 |
| --- | --- |
| [mdr-protocol README](../../../references/supplemental/01-protocol-docs/mdr-protocol/README.md) | Tandem 帧、转义、序号、ACK 与校验；来自旧设备研究 |
| [MDR 消息分类](../../../references/supplemental/01-protocol-docs/mdr-protocol/MDR_packets.md) | MDR 命令分类；不是 XM5 的完整规格 |
| [逆向方法](../../../references/supplemental/01-protocol-docs/mdr-protocol/reverse_engineering.md) | 如何对照官方 App 操作与蓝牙数据 |
| [ohm 协议笔记](../../../references/supplemental/01-protocol-docs/sony-headphones-bluetooth-documentation/docs/Protocol/Specification.md) | RFCOMM 数据分析；未知项原样保留 |
| [SonyHeadphonesClient V2/T2 协议头](../../../references/SonyHeadphonesClient/libmdr/include/mdr/ProtocolV2T2.hpp) | 与较新耳机功能相关的定义入口 |
| [公开 C 接口](../../../references/SonyHeadphonesClient/libmdr/include/mdr-c/Headphones.h) | 功能、状态、能力和结果读取接口 |
| [协议生成工具说明](../../../references/SonyHeadphonesClient/tooling/README.md) | 生成协议、序列化与校验代码的流程 |
| [上游贡献规则](../../../references/SonyHeadphonesClient/.github/CONTRIBUTING.md) | 协议头应由工具生成，避免手改导致不一致 |

## 读源码时分清层次

Windows Bluetooth / RFCOMM 负责连接和字节流；Tandem 负责封包与 ACK；MDR 负责耳机命令。抓包中的 RFCOMM 外层字段不应重复拼进应用的 MDR 数据包。旧设备服务 UUID 也不能直接套用到 XM5，应对照设备协议代际、服务发现与现有 Windows 实现。

当前 SonyHeadphonesClient 的 IR 来源是官方 iOS 二进制，不是项目保存的 Android APK；具体说明见 [05 APK 分析工具](../05-APK分析工具/README.md)。

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
