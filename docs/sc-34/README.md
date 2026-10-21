# SC-34：单 EXE 分发与完整许可内置

2026-10-21。分支 `FLEETING/sc-34-single-exe`，基于 `main` 的 `2f0e80a`；修改前已 fetch Gitee，`main...gitee/main` 为 0/0。保留工作区已有 AGENTS.md、任务状态及目录整理文档修改。本轮风险等级 **L2**，未改蓝牙协议或连接控制。

## 实现

- CMake 将更新源 JSON 和 notices 中全部 8 份原始文件作为 RCDATA 内置；自动枚举新增文件，并明确登记资源内容与对话框的增量构建依赖。
- 程序内三个更新源加载入口统一使用内置 JSON，复用原解析、地址校验、异步检查、渠道偏好和错误处理；不再读取 EXE 旁 config，未包含令牌。
- 「更多 → 第三方许可」提供本机离线阅读窗口，支持滚动、文本选择、Ctrl+A、Ctrl+C、复制全部、调整大小和关闭后再打开。无需设备连接；正常运行时也可从托盘进入更多功能。
- CMake install 和 ZIP 内仅有 `SonyConnect.exe`；说明、校验文件、测试报告放在打包输出根目录，不是运行依赖。源码中的 notices/config 保留，旧发布附件未修改。
- 完整保留 MIT、Lucide 的 ISC/Feather MIT、React Bits MIT + Commons Clause 原文和项目对应关系说明。没有将署名改成仅外部链接，也没有将组件拆分为独立组件库分发。保持现有项目授权边界，不新增根许可证。
- 用户偏好与日志继续写入 `%LOCALAPPDATA%\SonyConnect`；更新地址改变需重新发布 EXE。README 和便携版说明同步，区分旧 v0.1.0 发布包与本轮未发布候选。

## 实际验证

- Release 增量构建通过，无编译警告或错误；未执行 Debug 构建或整个项目回归。
- `update_tests.exe`：33 项离线更新用例通过，覆盖内置默认值、无效 schema 和现有渠道错误处理。
- `more_window_tests.exe artifacts/sc-34/more-window-tests`：35 项离线页面用例通过，报告和页面 PNG 在该目录。
- `Publish-Native.ps1 -SkipBuild`：在独立 `portable` 目录只有一个 EXE 的情况下通过 106 项自检：distribution 19、update 13、slider 62、diagnostics 12。
- 内置 8 份许可和更新 JSON 从正式 EXE 导出后逐份做 SHA-256，与源文件字节完全一致；ZIP 完整性和 EXE 哈希核对通过。
- 新许可窗口检查覆盖全文、只读、滚动、全选、真实剪贴板复制并恢复原内容、520×380/900×680 两档窗口大小，以及按钮和说明文字布局。
- 桌面剪贴板/截图测试在沙箱中分别触发 ole32 异常、Windows 错误 5；经定位后改用正常桌面权限，同一功能检查已通过。未通过的早期报告不作为验收依据。
- 设计预览底栏检查发现示例提示与许可入口重叠，已分配独立区域；最终更多页面定向测试通过。未据预览或离线用例宣称实机验证。

## 最终候选

- EXE：`C:\Users\FLEETING\Documents\Project\SonyConnect\artifacts\native\packages\20261021-022740-325\portable\SonyConnect.exe`
- EXE SHA-256：`990f9961f2f0cccfa8f6e0c884d280c288c2d09800fbb90be07cb1bbd5944b66`
- ZIP：`C:\Users\FLEETING\Documents\Project\SonyConnect\artifacts\native\packages\20261021-022740-325\SonyConnect-v0.1.0-Windows-x64.zip`
- 报告：`artifacts/native/packages/20261021-022740-325/summary.json`
- 本地候选版本仍为 0.1.0；未替换线上旧 v0.1.0，未公开发布，未提交或推送。

## 原始许可清单

以下文件原字节均在 EXE 中保留，阅读窗口只为显示规范化换行。

| 原文件 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `Lucide-LICENSE.txt` | 3208 | `b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57` |
| `React-Bits-LICENSE.md` | 1300 | `bae5d1ab75f63c9543b6c92d817480f355f7095fd2d95368ddcc886099a9e363` |
| `React-Bits-NOTICE.txt` | 1532 | `616ebde16f87c6fd9139cdf555767f23ed6dc3c95e6165a975820393e3d9f780` |
| `sony-device-center-MIT.txt` | 1157 | `006da6d371425ea1b93fe35cc4222e10482c027fd29e214c1d8cc4bccf490212` |
| `sony-tray-MIT.txt` | 1114 | `e2fb2e96c1dc263c78cb704f3c06a86ac970cd6041ea5321c4e260f545157a81` |
| `SonyConnect-native-NOTICE.txt` | 1686 | `8891384e0b7d557291be66813a9a263a0f9c935eaa840a47d8c2a803fa77906e` |
| `SonyConnect-Pen-icons-NOTICE.txt` | 1306 | `08ba2770fe952f72002eb6d2b75957463d9535d4162b2ba635ae8d0b01acaa67` |
| `SonyHeadphonesClient-MIT.txt` | 1120 | `915e766600639d41714a1c8b0e09e49e0ac3e8f3c90264c8e08d70195bc485c1` |

## 未验证与后续

- 本轮不调用蓝牙或外部网络；真实在线检查、系统浏览器打开、干净 Windows 和实际跨屏 DPI 变化未新增验证。更新 URL 路由及错误处理由定向离线测试验证。
- 用户提供的 v0.1.0 截图显示 WH-CH720N、固件 1.0.8，提示“耳机控制连接失败 / ACK or device response timed out”。这只证明控制连接超时，不能据此判断协议版本、蓝牙传输或手机抢占等具体原因；不将 SC-34 当作该问题的修复。待用户启动 SC-32 后单独排查，当前未修改 SC-32 状态或连接协议。
