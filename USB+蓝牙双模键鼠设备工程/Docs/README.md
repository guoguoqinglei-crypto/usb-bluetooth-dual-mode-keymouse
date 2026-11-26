# 双模键鼠设备（USB + 蓝牙）— 速查卡

> 完整设计文档见同目录 `USB键鼠设备设计文档.html`（含原理框图与软件流程图，可打印成 PDF）。
> 桌面上另有同内容的 `USB键鼠设备设计文档.pdf` 与 `.docx`。

## 1 这个项目是什么

同一套板载按键/摇杆，**同时**驱动两条独立的 HID 链路：

| 链路 | 协议栈 | 电脑上看到 | 物理接口 |
|---|---|---|---|
| USB | TinyUSB 三接口复合 HID | 键盘 + 鼠标 + 自定义 HID | PA11 / PA12，VID/PID `0xCafe / 0x4004` |
| 蓝牙 | RDA5876 + 裸 HCI 主机栈 | 蓝牙键盘 + 蓝牙鼠标 | USART2 PA2/PA3，设备名 `STM32 BT KeyMouse` |

USB 部分仍满足考核课题一（3 个 HID 接口、Caps/Num Lock 与指示灯、自定义 HID）。
声卡 / Codec / I²S / CDC 虚拟串口依旧不参与启动。

- USB 端点：键盘 IN `0x81`、鼠标 IN `0x82`、自定义 IN `0x83` + OUT `0x03`；配置描述符 91B；PMA 448/512
- 蓝牙 SDP：AttributeList 455 字节，内含 119 字节键鼠报告描述符（键盘 65B + 鼠标 54B），HID 子类 `0xC0`（键鼠组合）
- 蓝牙 CoD：`0x0005C0`（外设 / 键盘+指向设备组合），Link Key 存 Flash 末页 `0x0803F800`

## 2 自动选路（防止双倍输入）

`BT_Task()` 每次发报告前查 `UsbHid_IsMounted()`：

- **USB 已枚举完成** → 输入只走 USB；蓝牙链路照常保持连接、照常跑 L2CAP，但**不发输入报告**
- **USB 未挂载** → 输入走蓝牙键鼠
- 串口会打印 `[mux] input -> USB (BT reports held)` / `[mux] input -> BLUETOOTH`

> ⚠ **当前 `BT_FORCE_REPORT = 1`（排查状态）**：不管 USB 挂没挂，蓝牙都照发。
> 目的是排除"USB 把蓝牙报告挡掉"这一种可能。代价是两条链路连同一台电脑时
> 会有双份输入（光标快一倍、按键重复），这是**正常现象**，不是新故障。
> 蓝牙确认可用后改回 `0` 即恢复自动选路。此时日志打印
> `[mux] BT FORCED (USB mounted too -> double input)`。

> ⚠ 如果板子只靠这根 USB 线供电，拔线会一起断电，蓝牙也就无法工作。要验证"纯蓝牙"必须给板子独立供电（ST-Link / 外部 5V / 电池）。

## 3 改动的文件

| 文件 | 作用 |
|---|---|
| `User/main.c` | 双模启动入口；`ENABLE_BLUETOOTH` 总开关（改 0 退回纯 USB 单模） |
| `hardware/TinyUSB/usb_hid.h/.c` | 三接口 HID、1ms 时基，新增 `UsbHid_IsMounted()` 供选路 |
| `hardware/bt_sdp_record.h` | 键鼠描述符 + 4 个长度字段重算；服务名改 `STM32 HID KeyMouse Dev`、PnP VID/PID 改 `0x1234/0x4004`（原来抄自 Xbox 手柄 `0x045E/0x02E0`） |
| `hardware/bt_gamepad.c` | 文件名沿用旧称，内容已改为键鼠报告组装 + 自动选路；`BT_FORCE_REPORT` 调试开关 |
| `hardware/bt_cmd.c` | 设备名 `STM32 BT KeyMouse`、CoD `0x0005C0` |
| `hardware/bt_l2cap.c` | HIDP 应答：`SET_PROTOCOL` 两种协议都接受并记录、`GET_PROTOCOL` 如实回报、`GET_IDLE` 补全数据、`GET_REPORT` 按当前协议回空 DATA |
| `hardware/bt_hci.c` / `bt_evt.c` / `Init_rda5876.c` / `bt_uart.c` | 原有可用蓝牙栈，未改动 |

`Project.uvprojx` **无需改动**：蓝牙栈的 `BtApp` 文件组早就存在于工程中，这次只是把 `main.c` 接上并换掉报告内容。

## 4 编译与烧录

```text
工程：  D:\minzhan\stm32f103rct6工程模板\stm32f103rct6工程模板\Project.uvprojx
编译：  D:\k5\UV4\UV4.exe -r Project.uvprojx -j0 -o build_dual.log
结果：  0 Error / 490 Warning，Code=56620 RO=5732 RW=56 ZI=11528
        Project.hex 已生成（双模版，含 HIDP 双协议 + SDP 标识修正）
串口：  921600 8N1（USART1）。先开串口再按 RESET
```

RO-data 比纯 USB 版多约 4.5KB，是 RDA5876 的五张补丁表；ZI 增加主要是蓝牙环形缓冲与 L2CAP 状态。
490 条告警全部来自 TinyUSB 的 C11/pedantic 提示，与改动前完全一致。

**启动顺序**（`main.c`）：D+ 拉低 → 串口/TIM4 → 按键/摇杆/RGB → 蓝牙初始化（阻塞约 1.5s）→ `UsbHid_Init()` 放开 D+ 开始枚举 → 主循环同时喂 `UsbHid_Task()` 与 `BT_Task()`。蓝牙初始化是阻塞的，必须先让主机看不见 USB。

## 5 键位表（两条链路一致）

| 板载输入 | USB / 蓝牙标准功能 | 自定义 HID 位图 |
|---|---|---|
| A | 鼠标左键 | bit0 |
| B | 鼠标右键 | bit1 |
| X | Caps Lock（0x39） | bit2 |
| Y | Num Lock（0x53） | bit3 |
| UP | 键盘空格 Space（0x2C） | bit4 |
| DN | 键盘回车 Enter（0x28） | bit5 |
| LF | 鼠标中键 | bit6 |
| RG | 仅 USB 自定义 HID 上报；蓝牙侧不使用 | bit7 |
| 左摇杆 | 光标相对位移（90° 安装旋转修正） | — |
| 右摇杆 | 滚轮（唯一来源，100ms 一格） | — |

**键盘按键语义（两侧一致）**：

- UP / DN 是**状态型**：按下后报告里一直挂着空格 / 回车键码，松手才发全 0 报告释放。长按连续输入由主机键盘连发完成（Windows 默认约 0.5s 后每 30ms 一个），和真键盘手感一致。
- X / Y 是**点按型**：按下 30ms 后主动补发释放报告。CapsLock / NumLock 是切换键，按住不放会被系统反复切换。
- 蓝牙侧同样是状态型报告：按键位图变化才发一次，按住时保持该键码，Windows 按自身重复率连发。

**指示灯**：USB 键盘 LED 输出报告同步板载 RGB —— 绿灯 = Num Lock，红灯 = Caps Lock。蓝牙侧的 `SET_REPORT`（LED）目前只回成功、不解析，所以纯蓝牙模式下灯不跟随锁定状态。

**摇杆方向与速度怎么调**：`usb_hid.c` 的 `USB_JOY_*` 和 `bt_gamepad.c` 的 `BT_JOY_*` 是两套相同的宏，**改要一起改**。

| 现象 | 改哪个宏 |
|---|---|
| 推「上」光标却往左跑 | `USB/BT_JOY_AXIS_ROTATE` 保持 `1`，否则依次试 `0 / 2 / 3` |
| 左右反 / 上下反（仅 USB） | `USB_MOUSE_INVERT_X` / `USB_MOUSE_INVERT_Y` 改成 `1` |
| 滚轮方向反（仅 USB） | `USB_WHEEL_INVERT` 改成 `1` |
| 光标太快 | 调小两套 `*_MOUSE_MAX`（现为 `12`，满偏约 1200 像素/秒） |
| 松手光标自己漂 | 调大两套 `*_DEADZONE`（默认 `3000`） |

## 6 蓝牙键鼠报告格式

SDP 里两个 Application Collection，靠 Report ID 区分；HIDP 输入报告统一是 `0xA1 + ID + 数据`：

```text
Report 协议（主机未切 Boot 时）
ID 1 键盘（10 字节）：A1 01 [修饰键][保留][6 键码]
ID 2 鼠标（ 6 字节）：A1 02 [按键位][X][Y][滚轮]

Boot 协议（Windows 蓝牙键盘驱动通常切到这个）
键盘（ 9 字节）：A1 [修饰键][保留][6 键码]      ← 无 Report ID
鼠标（ 4 字节）：A1 [按键位][X][Y]              ← 无 Report ID，boot 鼠标无滚轮
```

**为什么要两套**：Windows 的 `hidbth` 驱动会把蓝牙键盘切到 Boot 协议以兼容 BIOS / 登录界面。
如果设备拒绝 `SET_PROTOCOL(Boot)`，部分 Windows 版本会判定"设备不合规"而**不加载键鼠驱动**
——现象正是「蓝牙连得上、设备也在，但按什么都没反应」。固件按 `g_bt_hid_protocol`
自动切换报告格式（**协议值：0 = Boot，1 = Report**，与 HID Profile 一致），串口会打印
`[hid] SET_PROTOCOL(0) -> Boot` 或 `[hid] SET_PROTOCOL(1) -> Report`。

**2026-11-17 修复的根因（重要）**：上一版把 HIDP 事务编号表整体写错——
`0x52 SET_REPORT(Output)`（Windows 下发键盘 LED）被误当成 `SET_PROTOCOL` 执行，
`g_bt_hid_protocol` 被写成非法值 2，之后所有键鼠报告格式全错，Windows 全部丢弃，
正是"连得上但完全没反应"的直接原因。现在事务号按 HID Profile 表 3.1 正确分派：
`4=GET_REPORT 5=SET_REPORT 6=GET_PROTOCOL 7=SET_PROTOCOL 8=GET_IDLE 9=SET_IDLE`，
`GET_PROTOCOL` 响应改为两字节 `A0 + 协议值`，Boot 鼠标报告单独组包（不再截短 Report 数组）。

通道一建立还会**主动补发一帧全松开报告**：部分主机要收到第一帧输入才真正注册设备。

键盘输出报告（LED）现在会真正解析：`SET_REPORT(Output)` 和 Interrupt 通道的 `0xA2`
输出报告都调用 `bt_app_set_keyboard_leds()` 点亮板载红/绿灯，与 USB 侧行为一致；
`GET_REPORT(Output)` 回当前 LED 状态。
设备每 20ms 采样一次；键盘只在位图变化时发，鼠标在位移/滚轮非零或按键变化时发。

## 7 自定义 HID 协议（仅 USB，8 字节，无 Report ID）

```text
输入（板子 → PC）：[0]=按键位图（1=按下）  [1]=报告序号  [2..7]=0
                   变化即上传，无变化每 1000ms 一次心跳
输出（PC → 板子）：[0]=0x01 SET_RGB  [1]=R  [2]=G  [3]=B  （各 0~100）
                   [0]=0x02 GET_STATUS 为预留命令，当前只打印日志

示例  05 1C 00 00 00 00 00 00   → A 与 X 按下，序号 0x1C
      01 64 32 00 00 00 00 00   → 红 100 / 绿 50 / 蓝 0（橙色）
```

上位机要打开的是**第三个 HID 接口**（VID `0xCafe` / PID `0x4004`），不要误开系统键盘或鼠标接口。
Windows 的 HID API 在无 Report ID 设备上可能要求首字节补 `0x00`，读取时也可能带回一个前导 0。

## 8 验证清单

USB 侧：

- [ ] 设备管理器出现键盘、鼠标、自定义 HID 三个设备，无「未知 USB 设备」
- [ ] 串口依次打印 `[bt] init=OK`、`[usb] enumeration released`、`[usb] mounted`
- [ ] 左摇杆四方向对应光标，中心附近精细定位、松手即停
- [ ] A / B / LF 点击；右摇杆滚动；X / Y 切换锁定状态且红/绿灯同步
- [ ] UP / DN 出现空格 / 换行；**按住不放能连续输入**，串口打印 `kbd hold 0x2C` / `0x28`，松手打印 `kbd hold 0x00`
- [ ] 自定义 HID 上行位图、下行 RGB 命令（需上位机或 HID 调试工具）

双模 / 蓝牙侧（**本次改动尚未上板实测**）：

- [ ] 独立供电、不插 USB 时打印 `[mux] input -> BLUETOOTH`
- [ ] 串口出现 `[hid] SET_PROTOCOL(0) -> Boot` 或 `SET_PROTOCOL(1) -> Report`，状态行 `proto` 只为 0 或 1
- [ ] 收到 `0x52 SET_REPORT(Output)` 后协议**不变**，并打印 `[bt] led num=.. caps=..`
- [ ] 通道建立后立刻出现 `[bt] HID interrupt channel open`，`rpt=` 从 0 开始增长
- [ ] Windows 蓝牙设置里能搜到 `STM32 BT KeyMouse` 并配对成功
- [ ] 设备管理器出现蓝牙键盘 + 蓝牙鼠标（不是游戏手柄），无「无可用服务」
- [ ] 蓝牙链路下光标、左右键、滚轮、Caps/Num Lock、空格/回车全部生效
- [ ] 断电再上电能自动回连（Link Key 存 Flash）

## 9 「电脑能连上蓝牙，但键鼠没反应」排查清单

按顺序做，每一步都能用串口日志判断：

**① 先在 Windows 里删掉旧配对（最常见）**
这台电脑之前配对过蓝牙**游戏手柄**，BD_ADDR 没变，Windows 会沿用缓存的设备类型和驱动，于是 SDP 里换成的键盘/鼠标描述符不被加载。
设置 → 蓝牙和其他设备 → 找到旧设备 →「移除设备」；板子**上电时按住 A 键**清掉 Flash 里的绑定（串口打印 `[bond] CLEARED`）；再重新搜索配对。

**② 确认 USB 数据线没插在电脑上**
只要 USB 枚举成功，`UsbHid_IsMounted()=1`，自动选路会**故意不发蓝牙报告**（否则同一台电脑双倍光标）。
验证纯蓝牙最好用「只供电、不接 D+/D−」的线（贴住 A 口中间两触点，或买充电专用线）接电脑/充电头。
当前固件已把 `BT_FORCE_REPORT` 设成 `1`（强制发），所以这一条暂时不挡人 —— 但确认可用后要改回 `0`。
状态行每 2 秒打印一次，`usb=` 就是主机是否枚举成功、`proto=` 是当前 HID 协议：

```text
[bt] st=3 ch=111 usb=0 proto=0(Boot) rpt=128 t=15320 drop=0
         │      │  │     │    │              │        └ 串口丢字节数（应为 0）
         │      │  │     │    │              └ 连接已持续毫秒
         │      │  │     │    └ 已发报告数（不动 = 报告没出去）
         │      │  │     └ 协议：0=Boot，1=Report（出现别的值 = 固件出 bug）
         │      │  └ USB 是否挂载
         │      └ SDP / HID-Control / HID-Interrupt 三个通道
         └ 0=初始化 1=可发现 2=已连接 3=HID 就绪
```

| 日志 | 结论 |
|---|---|
| `[mux] input -> USB (BT reports held)` | 选路挡住了，换供电方式 |
| `[mux] input -> BLUETOOTH` 或 `BT FORCED` | 选路没问题，往下查 |
| `st=3` 但 `rpt=` 不涨 | 报告没发出去，看 `[hid]` 行有没有 SET_PROTOCOL |
| `st=3 ch=110` | HID Interrupt 通道没开，主机没认出 HID 设备（SDP / 驱动问题） |
| `st=3 ch=100` | 只开了 SDP，HID Control 没配好 |
| `st=2 ch=000` | 主机没来开通道，等回连逻辑主动发起 |
| 反复出现 `[bt] init=OK stage=...` 从头开始 | 板子被断电重启，查供电（充电宝轻载关机） |

**③ 看设备管理器**
应出现「蓝牙 HID 键盘设备」和「HID 鼠标」，不能是「游戏控制器」或带黄色感叹号。固件已把 SDP 服务名从 `Xbox Bluetooth Gamepad` 改成 `STM32 HID KeyMouse Dev`、PnP VID/PID 从 `0x045E/0x02E0` 改成 `0x1234/0x4004`，就是为了避免 Windows 套用 Xbox 手柄驱动。

**④ HIDP 事务分派（2026-11-17 修复的根因）**
上一版把事务编号表写错：`0x52 SET_REPORT(Output)`（键盘 LED）被误判成 `SET_PROTOCOL`，
协议状态被写成非法的 2，之后所有报告格式错误、被 Windows 丢弃。
现在按 HID Profile 表 3.1 正确分派（4=GET_REPORT / 5=SET_REPORT / 6=GET_PROTOCOL /
7=SET_PROTOCOL / 8=GET_IDLE / 9=SET_IDLE），协议值统一为 **0=Boot、1=Report**，
断线重连自动恢复 Report，`SET_REPORT(Output)` 只更新 LED 不碰协议。

**⑤ 供电别拖后腿**
充电宝有轻载自动关机（板子 5V 侧仅 30~60mA，低于 50~80mA 阈值），表现为「上电几秒就断电重启」，串口会反复从头打印启动信息。改用电脑 USB 口 / 5V 充电头（配充电专用线），或让充电宝进小电流模式、并 47Ω/1W 假负载。

## 10 待办与风险

- **PC 上位机暂未实现**（用户 22:34 要求暂缓），对应考核第 3 项 30 分；协议已在第 7 节固定。
- **HIDP 事务表 / 协议值 / Boot 鼠标格式 / LED 解析已于 2026-11-17 修复，编译通过但尚未上板实测**。
  日志判别：收到 `0x52` 后协议不变；`proto` 只出现 0 或 1；`[bt] led num=.. caps=..` 出现即 LED 链路通。
- HCI 层顺带修复：`Host_Number_Of_Completed_Packets` 参数补足 5 字节、初始化时打开
  `Set_Controller_To_Host_Flow_Control(1)`、ACL 分片按片扣信用（原来按 PDU 只扣 1）。

## 11 回退

| 想回到 | 做法 |
|---|---|
| 纯 USB 单模（保留当前文件） | `User/main.c` 里 `#define ENABLE_BLUETOOTH 0`，重编译 |
| 本次改动前的 USB V1.3 版 | 用 `D:\minzhan\_backup_usb_keymouse_20261116_2350\` 覆盖工程（930 文件，含 `Project.hex`） |
| 已验证的蓝牙游戏手柄 | 用 `D:\minzhan\_backup_bt_gamepad_20261116_213104\` 覆盖 `User/`、`hardware/`、`Project.uvprojx`、`Project.hex`，重编译应为 `Code=64688` |
