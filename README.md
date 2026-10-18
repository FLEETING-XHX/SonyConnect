# SonyConnect

在 Windows 电脑上调整 Sony 耳机的降噪、环境声音、均衡器等设置。支持托盘操作、双设备音源切换和播放控制，具体功能以耳机支持情况为准。

本项目为非官方工具，与 Sony 公司无隶属关系。

## 下载

**[GitHub 下载](https://github.com/FLEETING-XHX/SonyConnect/releases/latest) · [Gitee 下载](https://gitee.com/fleetingxhx/SonyConnect/releases)**

请选择发行版附件中的 `SonyConnect-v版本号-Windows-x64.zip`，不要下载 Source code 源码包。

## 使用方法

1. 解压下载的 ZIP，保留里面的所有文件和文件夹。
2. 在 Windows 蓝牙设置中配对并连接耳机。
3. 双击 `SonyConnect.exe`，即可开始使用。未找到耳机时，点击主界面右侧的刷新按钮。
4. 程序隐藏后，单击任务栏右下角的托盘图标可重新打开；右键图标可以切换声音模式或退出程序。

首次使用不需要安装程序，也不需要另外安装 .NET 或 Visual C++ 运行库。**请完整解压使用，不要只复制 EXE**：`config` 中的文件用于检查更新，`notices` 中保留第三方说明。

电脑与手机可以同时连接耳机；调整耳机设置时，建议交替使用本程序和手机 Sony App，避免两个控制程序同时修改设置。

## 支持范围

- **电脑：**Windows 10 / 11 的 64 位 Intel 或 AMD 电脑，需要可正常使用的蓝牙及驱动。
- **耳机：**目前已完成 WF-1000XM5 的当前范围验收。WH-1000XM5、LinkBuds S、WH-CH720N 等型号仍待独立适配与验证。
- 不支持的功能会显示为不可用；不能保证所有电脑、耳机型号和固件都能正常使用。当前发布包不面向 Windows 7 / 8、32 位系统或 ARM 设备提供兼容保证。

## 更新与卸载

- 在「更多 → 诊断与维护」中检查更新，也可以直接访问上方下载页面。程序不会自动替换安装。
- 更新前先从托盘退出旧版，再解压新版使用。不要同时运行两个版本。
- 卸载时先关闭开机启动，再退出程序，删除解压目录即可。设置和日志保存在 `%LOCALAPPDATA%\SonyConnect`，可按需自行保留或删除。

## 问题反馈

请在 [GitHub Issues](https://github.com/FLEETING-XHX/SonyConnect/issues) 或 [Gitee Issues](https://gitee.com/fleetingxhx/SonyConnect/issues) 提供耳机型号、Windows 版本、问题描述及复现步骤。需要时可从「诊断与维护」导出诊断包，分享前请核对内容。

更多说明见 [项目文档](docs/README.md)，第三方说明随下载包提供。
