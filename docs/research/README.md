# SonyConnect 补充资料总目录

归档日期：2026-10-09。这里整理开发 SonyConnect 值得保留的协议、源码、设备支持与调试资料。入口均指向本地文件；网页原始来源、下载时间、大小与 SHA-256，仓库分支与提交见 [资料清单](资料清单.json)。

## 分类入口

| 分类 | 内容 |
| --- | --- |
| [01 协议文档](01-协议文档/README.md) | Tandem / MDR 封包、旧协议逆向记录、协议版本和生成头文件入口 |
| [02 设备支持](02-设备支持/README.md) | WF / WH-1000XM5 上游支持表、Gadgetbridge 支持页面和型号能力源码 |
| [03 双设备与固定播放](03-双设备与固定播放/README.md) | PR #57 离线讨论、补丁、字段含义及拒绝场景 |
| [04 抓包与调试](04-抓包与调试/README.md) | Android HCI 日志、离线回放、模拟传输、问题定位记录模板 |
| [05 APK 分析工具](05-APK分析工具/README.md) | JADX 官方说明与许可、已有 APK、上游 iOS 协议提取流程 |
| [06 Windows 开发](06-Windows开发/README.md) | WPF、RFCOMM、系统音量、媒体控制、快捷键、便携发布、性能和更新检测 |

## 新增仓库的位置

| 仓库 | 本地入口 | 定位 |
| --- | --- | --- |
| AndreasOlofsson/mdr-protocol | [README](../../references/supplemental/01-protocol-docs/mdr-protocol/README.md) | 旧设备封包基础与逆向方法；文档不完整 |
| ohm-app/sony-headphones-bluetooth-documentation | [文档首页](../../references/supplemental/01-protocol-docs/sony-headphones-bluetooth-documentation/docs/index.md) | 研究中的独立协议笔记；有未知字段 |
| AmitRajput-Dev/SonyBridge | [README](../../references/supplemental/02-desktop-implementations/SonyBridge/README.md) | 另一套桌面实现；保留 imgui 子模块 |
| Freeyourgadget/Gadgetbridge | [README](../../references/supplemental/03-android-implementation/Gadgetbridge/README.md) | Sony 耳机相关 Android 协议、型号能力和测试 |

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
