# 04 抓包与调试

| 本地入口 | 用途 |
| --- | --- |
| [Android 官方 HCI 日志指南](Android-蓝牙HCI日志.html) | 官方 App 操作对应的蓝牙交换；日志导出按手机系统确认 |
| [SonyHeadphonesClient 记录与回放指南](../../../references/SonyHeadphonesClient/tests/README.md) | record、replay、协议调试器与回归测试 |
| [WF-1000XM5 固件 6.1.0 样本目录](../../../references/SonyHeadphonesClient/tests/WF-1000XM5-6.1.0/) | 已在原仓库保留的接收数据包 |
| [抓包数据清理工具源码](../../../references/SonyHeadphonesClient/tooling/scrub-capture.py) | 保持长度与校验规则的隐私字段处理参考 |
| [sony-tray 模拟传输](../../../references/sony-tray/tests/SonyTray.Tests/FakeTransport.cs) | 离线模拟连接、发送与接收 |
| [sony-device-center 架构](../../../references/sony-device-center/docs/architecture-current.md) | 协议、设备状态和界面分层 |
| [IPC 与生命周期](../../../references/sony-device-center/docs/ipc-and-lifecycle.md) | 连接生命周期、进程与事件处理 |
| [技术债记录](../../../references/sony-device-center/docs/technical-debt.md) | 未完成范围和故障处理线索 |

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
