# 06 Windows 开发

以下文件为官方文档正文快照，来源和校验值见资料清单。

| 离线文档 | 与 SonyConnect 的对应关系 |
| --- | --- |
| [WPF 概览](WPF-概览.html) | C# 桌面 UI 与控件基础 |
| [RFCOMM 接口](Windows-RFCOMM接口.html) | Windows 蓝牙控制连接 |
| [系统音量接口](Windows-系统音量接口.html) | IAudioEndpointVolume：系统音频端点音量与变化通知 |
| [媒体会话接口](Windows-媒体会话接口.html) | 播放 / 暂停、曲目或视频信息；取决于媒体应用提供的会话 |
| [全局快捷键](Windows-全局快捷键.html) | RegisterHotKey、快捷键注册与冲突处理 |
| [蓝牙设备类型](Windows-蓝牙设备类型.html) | Class of Device 与设备大类 |
| [NET 单文件发布](NET-单文件发布.html) | 单文件、自包含与运行时文件提取 |
| [NET 裁剪不兼容情况](NET-裁剪不兼容情况.html) | WPF 等裁剪限制，避免盲目启用 Trim |
| [Visual Studio 性能分析](VisualStudio-性能分析.html) | CPU、内存、分配与性能分析 |
| [NET 运行时计数器](NET-运行时计数器.html) | 运行时指标与 dotnet-counters 工具说明 |
| [GitHub Releases 接口](GitHub-Releases接口.html) | 后续检测更新与 Releases 跳转的接口资料 |

## 实现边界备忘

- Windows 看到的设备分类可辅助 PC / 手机图标；耳机返回的远端设备列表是否带类型需另外核对。Windows 本机 API 不保证能读取耳机连接的另一台手机的完整信息，未知设备保留通用图标与手动更正入口的可能性。
- 系统音量、耳机内部音量和当前活动音源分别建模。媒体控制与标题来自可用会话，遇到不支持的播放器提供中性占位，不能假定任何视频都可读取名称。
- 默认快捷键已改为 R-Ctrl+F1～F3，Reset 恢复右 Ctrl 组合。RegisterHotKey 的 MOD_CONTROL 不区分左右 Ctrl，指定右侧需要额外的按键识别路径；实现与冲突处理仍需验证。
- 耳机连接质量偏好不等于强制系统使用某个 codec；当前活动来源和两端能力一起决定协商结果。SBC / AAC / LDAC 的显示依靠实际可读取状态。Gadgetbridge 的 AudioCodec 枚举提供交叉核对线索，不硬编码“稳定优先总是 AAC”。
- 单 EXE、自包含和免安装的目标仍要测量发布体积、空闲内存、托盘状态与动画时的 CPU。WPF 的裁剪限制见官方文档；体积和占用本轮没有实测数值。
- React Bits 只作为动效参考；WPF 需实现相应的动画与交互，不能直接把 React 组件当 WPF 控件。动画只在交互时运行，后台与隐藏窗口停止无必要的更新。
- 本项目仓库已创建：https://github.com/FLEETING-XHX/SonyConnect，发布页为该仓库 /releases，目标配置见 ../../../config/update-source.json。仓库目前私有，匿名 API 查询不可视为已支持；检测逻辑、红色版本提示与点击跳转待应用开发。

本轮没有安装开发环境或执行上述工具；具体依赖与发布参数在开始实现时核对。
