# 02 设备支持

| 本地入口 | 核对内容 |
| --- | --- |
| [SonyHeadphonesClient WF-1000XM5 支持表](../../../references/SonyHeadphonesClient/docs/device-support/WF-1000XM5.md) | WF 功能列表、上游支持状态 |
| [SonyHeadphonesClient WH-1000XM5 支持表](../../../references/SonyHeadphonesClient/docs/device-support/WH-1000XM5.md) | WH 功能支持，与 WF 分开判断 |
| [sony-device-center 设备矩阵](../../../references/sony-device-center/docs/device-matrix.md) | 已测试 / 未测试范围 |
| [Gadgetbridge Sony 支持页面](Gadgetbridge-Sony支持情况.html) | 型号、缺项及相关 issue 的网页快照 |
| [Gadgetbridge WF-1000XM5 型号能力源码](../../../references/supplemental/03-android-implementation/Gadgetbridge/app/src/main/java/nodomain/freeyourgadget/gadgetbridge/devices/sony/headphones/coordinators/SonyWF1000XM5Coordinator.java) | 默认能力、左右耳与盒子电量、实验性状态 |
| [Gadgetbridge WH-1000XM5 型号能力源码](../../../references/supplemental/03-android-implementation/Gadgetbridge/app/src/main/java/nodomain/freeyourgadget/gadgetbridge/devices/sony/headphones/coordinators/SonyWH1000XM5Coordinator.java) | WH 默认能力 |
| [SonyBridge 支持范围](../../../references/supplemental/02-desktop-implementations/SonyBridge/README.md) | 另一客户端的实测与预期支持区分 |

已读取 Gadgetbridge 的 WF-1000XM5 coordinator：isExperimental() 返回 true，源码注释说明环境声控制未完全工作、耳机音量控制可能有问题。默认 capability 列表不是在本机动态查询得到的结果，不能据此把高级功能标成用户耳机已验证。

SonyConnect 应以连接时读到的协议版本与能力决定控件状态，并记录本机实测。耳机盒电量可能不是实时值，应结合设备实际返回与更新时间呈现。用户已取消的“通话过程中捕获语音”不因参考源码存在该功能而重新加入产品范围。
