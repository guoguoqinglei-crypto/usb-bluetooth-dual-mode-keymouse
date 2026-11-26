# USB＋蓝牙双模键鼠设备

基于 STM32F103RCT6、RDA5876、TinyUSB 和 FreeRTOS 的 USB／蓝牙双模键盘鼠标，包含 Windows Qt 上位机。

## 主要功能

- USB 复合 HID：标准键盘、标准鼠标和自定义 HID 三接口
- RDA5876 蓝牙 HID：键盘、鼠标和自定义状态／灯控通道
- FreeRTOS 三任务调度，长按 RG 2 秒切换 USB／蓝牙并保存偏好
- 双摇杆、8 键、Caps／Num Lock、RGB 手动调色与彩虹灯效
- 左摇杆按下 L3＝左 Shift，右摇杆按下 R3＝左 Ctrl；独立 20 ms 消抖状态机，USB／蓝牙均生效
- 自定义输入报告扩展为 9 字节，上位机新增 L3／R3 状态、计数与覆盖显示，并兼容旧 8 字节输入
- 上位机显示链路、按键、报告和诊断日志，支持 USB／蓝牙通信

## 仓库内容

- `USB+蓝牙双模键鼠设备工程/`：STM32 Keil 固件工程与源码。
- `USB+蓝牙双模键鼠设备上位机/`：Windows Qt 上位机可执行程序及运行库。
- `firmware/Project.hex`：可直接烧录的固件（源码中已提供时）。
- 根目录设计文档：项目原理、协议、流程、调试方法与测试结果。

## 构建与烧录

1. 使用 Keil µVision 打开 `USB+蓝牙双模键鼠设备工程/Project.uvprojx`。
2. 执行 Build／Rebuild，生成固件。
3. 也可直接烧录 `firmware/Project.hex`（若仓库包含该文件）。
4. 主控为 STM32F103RCT6；具体引脚、供电和接线以设计文档为准。

## 上位机

进入 `USB+蓝牙双模键鼠设备上位机/`，运行 `dist/shujianffuhe.exe`。请保留整个目录，不要只复制 EXE，以免缺少 Qt 插件或运行库。

## 设计文档

- `USB+蓝牙双模键鼠设备设计文档.docx`
- `USB+蓝牙双模键鼠设备设计文档.pdf`

## 开源许可

项目自有代码与文档采用 [MIT License](LICENSE)。第三方组件遵循各自原始许可证，详见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
