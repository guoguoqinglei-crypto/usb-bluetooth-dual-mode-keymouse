#include <bt_hci.h>

#include <string.h>
#include <stm32f10x.h>

#include <bt_uart.h>
#include <uart.h>
#include <delay.h>
#include <joystick.h>
#include <key.h>
#include <usb_hid.h>        /* hal_time_ms() 时基 + UsbHid_IsMounted() 自动选路 */
#include <Init_rda5876.h>   /* RDA_OK + Init_rda5876()：回连失败时重新初始化控制器 */
#include <tim2.h>           /* 键盘 LED -> 板载 RGB（NumLock/CapsLock 指示） */
#include <km_config.h>      /* 与 USB 共用的改键、摇杆响应和掉电配置 */

#define BOND_FLASH_ADDR   0x0803F800u
#define BOND_MAGIC        0x424F4E44u    /* "BOND" */

typedef struct
{
    uint32_t magic;
    uint8_t  addr[6];
    uint8_t  pad[2];
    uint8_t  key[16];
    uint32_t xorCheck;
} bond_t;

static uint8_t s_bondAddr[6];
static uint8_t s_bondKey[16];
static uint8_t s_bondValid = 0u;

static uint8_t  s_bondPending = 0u;      /* 有 Key 待落盘 */
static uint32_t s_bondStamp   = 0u;      /* Key 到达时刻 */

/* 回连状态：每次只推进一条 HCI 命令，等事件回来后再走下一步。 */
static uint8_t  s_reconnectOn  = 0u;
static uint8_t  s_reconnectBusy = 0u;
static uint8_t  s_scanOffPending = 0u;
static uint8_t  s_tryCount     = 0u;
static uint32_t s_lastTry      = 0u;

/* 采样 */
static uint32_t s_lastSample = 0u;
static uint32_t s_lastReport = 0u;
static uint32_t s_connMs     = 0u;      /* 本次连接的起点（状态行算 t= 用） */
static uint32_t s_hidCtrlMs  = 0u;      /* HID Control 配好的时刻（用来延时自己开 Interrupt） */
static uint8_t  s_byUs       = 0u;      /* 本次连接是不是我们主动回连的 */

#define SAMPLE_MS      10u
#define REPORT_MS      10u
#define RECONNECT_MS   6000u    /* 每次回连间隔 6 秒，失败不关闭后续回连 */
#define BOND_DELAY_MS  3000u

/* 协议复用 BT GAMEPAD 的连接与回连结构，报告内容只使用键鼠 ID1/2/3。 */
#define BT_REPORT_KEYBOARD  0x01u
#define BT_REPORT_MOUSE     0x02u
#define BT_REPORT_CUSTOM    0x03u
#define BT_CUSTOM_REPORT_LEN 9u    /* rep[8]：L3=左 Shift，R3=左 Ctrl */

#define HID_MODIFIER_LEFT_CTRL   0x01u
#define HID_MODIFIER_LEFT_SHIFT  0x02u

static void bt_reset_report_state(void);

static uint32_t bond_xor(const uint8_t *addr, const uint8_t *key)
{
    uint32_t v = BOND_MAGIC;
    uint8_t  i;

    for (i = 0u; i < 6u; i++)
    {
        v = (v << 1) ^ (uint32_t)addr[i];
    }
    for (i = 0u; i < 16u; i++)
    {
        v = (v << 1) ^ (uint32_t)key[i];
    }
    return v;
}

static void bond_load(void)
{
    const bond_t *b = (const bond_t *)BOND_FLASH_ADDR;

    s_bondValid = 0u;

    if (b->magic != BOND_MAGIC)
    {
        return;
    }
    if (b->xorCheck != bond_xor(b->addr, b->key))
    {
        return;
    }

    memcpy(s_bondAddr, b->addr, 6u);
    memcpy(s_bondKey, b->key, 16u);
    s_bondValid = 1u;

    /* s_bondAddr 存的是 HCI 线上的字节序（低字节在前），打印时反过来说 */
    UART1_Printf("[bond] found peer %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 s_bondAddr[5], s_bondAddr[4], s_bondAddr[3],
                 s_bondAddr[2], s_bondAddr[1], s_bondAddr[0]);
}

static void bond_save(void)
{
    bond_t   tmp;
    uint32_t *w;
    uint32_t  i;

    memset(&tmp, 0xFF, sizeof(tmp));
    tmp.magic = BOND_MAGIC;
    memcpy(tmp.addr, s_bondAddr, 6u);
    memcpy(tmp.key,  s_bondKey, 16u);
    tmp.xorCheck = bond_xor(s_bondAddr, s_bondKey);

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(BOND_FLASH_ADDR);

    w = (uint32_t *)&tmp;
    for (i = 0u; i < (uint32_t)(sizeof(tmp) / 4u); i++)
    {
        FLASH_ProgramWord(BOND_FLASH_ADDR + i * 4u, w[i]);
    }
    FLASH_Lock();

    UART1_Printf("[bond] saved to flash (%u bytes)\r\n", (unsigned)sizeof(tmp));
}

void bt_app_on_link_key(const uint8_t addr[6], const uint8_t key[16])
{
    memcpy(s_bondAddr, addr, 6u);
    memcpy(s_bondKey, key, 16u);
    s_bondValid   = 1u;
    s_bondPending = 1u;
    s_bondStamp   = 0u;              /* 由 Task 里补时间戳 */
}

uint8_t bt_app_link_key_for(const uint8_t addr[6], uint8_t out_key[16])
{
    if ((s_bondValid == 0u) || (memcmp(addr, s_bondAddr, 6u) != 0))
    {
        return 0u;
    }
    memcpy(out_key, s_bondKey, 16u);
    return 1u;
}

void bt_app_commit_bond(void)
{
    if (s_bondPending != 0u)
    {
        s_bondPending = 0u;
        bond_save();
    }
}

void bt_app_drop_bond(void)
{
    s_bondValid   = 0u;
    s_bondPending = 0u;
    s_bondStamp   = 0u;
    s_reconnectOn = 0u;
    s_reconnectBusy = 0u;
    s_scanOffPending = 0u;
    s_tryCount    = 0u;

    memset(s_bondAddr, 0, sizeof(s_bondAddr));
    memset(s_bondKey, 0, sizeof(s_bondKey));

    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPRTERR);
    FLASH_ErasePage(BOND_FLASH_ADDR);
    FLASH_Lock();

    UART1_SendString("[bond] CLEARED - ready to pair again\r\n");
}

void bt_app_on_connected(void)
{
    /* 此刻 g_conn_by_us 还没被事件层清零，正好存下来给加密完成时用 */
    s_byUs = g_conn_by_us;
    s_reconnectBusy = 0u;
    s_scanOffPending = 0u;
    s_tryCount = 0u;
    s_lastSample = 0u;
    s_lastReport = 0u;
    s_hidCtrlMs = 0u;
    s_connMs = hal_time_ms();
    Tim2_SetRedBrightness(0u);
    Tim2_SetGreenBrightness(0u);
    Tim2_SetBlueBrightness(0u);
    bt_reset_report_state();
}

void bt_app_on_peer_connect(void)
{
    /* 电脑已经主动寻呼，本机不能再并发 Create Connection。 */
    s_reconnectBusy = 0u;
    s_scanOffPending = 0u;
    s_byUs = 0u;
    g_conn_by_us = 0u;
}

void bt_app_on_connect_failed(uint8_t status)
{
    /* 无论失败来自 Command Status 还是 Connection Complete，都回到同一个
     * 可连接状态；下一周期会重新执行完整的分步回连。 */
    s_reconnectBusy = 0u;
    s_scanOffPending = 0u;
    s_reconnectOn = (s_bondValid != 0u) ? 1u : 0u;
    s_lastTry = hal_time_ms();
    g_conn_by_us = 0u;
    bt_cmd_scan_enable(0x03u);
    UART1_Printf("[bt] reconnect attempt ended, status=0x%02X; scan restored\r\n",
                 (unsigned)status);
}

void bt_app_on_scan_enable_complete(uint8_t status)
{
    if (s_scanOffPending == 0u)
    {
        return;                     /* 其它流程的 Scan Enable 完成事件 */
    }
    s_scanOffPending = 0u;

    if (status != BT_ERR_SUCCESS)
    {
        bt_app_on_connect_failed(status);
        return;
    }
    if ((s_bondValid == 0u) ||
        ((UsbHid_IsMounted() != 0u) && (UsbHid_IsBtPreferred() == 0u)))
    {
        s_reconnectBusy = 0u;
        s_lastTry = hal_time_ms();
        return;
    }

    /* 只有在关扫描的 Command Complete 把命令额度归还后，才发创建连接。 */
    g_conn_by_us = 1u;
    UART1_SendString("[bt] scan off complete -> create connection\r\n");
    bt_cmd_create_connection(s_bondAddr);
}

void bt_app_on_disconnected(void)
{
    /* 通道已关闭，直接丢弃保持状态；下次连接必须重新同步全量报告。 */
    bt_reset_report_state();
    s_reconnectBusy = 0u;
    s_scanOffPending = 0u;
    s_reconnectOn = (s_bondValid != 0u) ? 1u : 0u;
    s_tryCount = 0u;
    s_lastTry = hal_time_ms();
    s_hidCtrlMs = 0u;
    s_byUs = 0u;
    g_conn_by_us = 0u;

    /* 配对后若没等到 HID 就绪就掉线，也要把已收到的 Key 保存下来。 */
    if (s_bondPending != 0u)
    {
        bt_app_commit_bond();
    }

    /* USB 优先期间不需要恢复可连接扫描，避免无意义地占用 HCI 命令额度。
     * 只有独立供电或用户已选择蓝牙时才开启，后续由分步回连逻辑接管。 */
    if ((UsbHid_IsMounted() == 0u) || (UsbHid_IsBtPreferred() != 0u))
    {
        bt_cmd_scan_enable(0x03u);
    }
}

void bt_app_on_encrypted(void)
{
    /* 这里把扫描关掉：连接期间还开着查询扫描会占天线，下降包速率。 */
    bt_cmd_scan_enable(0x00u);

    if (s_byUs != 0u)
    {
        s_byUs = 0u;
        UART1_SendString("[bt] reconnect: we open HID Control\r\n");
        bt_l2cap_open_hid_control();
    }
}

/* 与 USB 使用完全相同的方向、8 次平均、死区和速度曲线。 */
#define BT_JOY_ROTATE          1
/* 死区：轻拨后摇杆回到的静止位置可能偏离标定中位，
 * 6000（18%）拦不住，实测光标会自己飘，放宽到 9000（27%）。
 * 代价是推动杆的起始行程略大，但静止时绝对不会再输出 ±1。 */
#define BT_JOY_DEADZONE        9000
#define BT_JOY_MOUSE_MAX       12
#define BT_WHEEL_THRESHOLD     12000
#define BT_WHEEL_INTERVAL      100u
#define BT_MOUSE_FILTER_SAMPLES 8u

static uint8_t  s_keyBm = 0u;
static uint8_t  s_keyModifier = 0u;
static uint8_t  s_mouseBtn = 0u;
static uint32_t s_wheelMs = 0u;
static uint8_t  s_routeUsb = 0xFFu;
static uint8_t  s_protoDirty = 1u;
static uint8_t  s_btActive = 0u;
static uint8_t  s_customBm = 0u;
static uint8_t  s_customJoy = 0u;
static uint8_t  s_customLed = 0xFFu;
static uint8_t  s_customSeq = 0u;
static uint32_t s_customBeat = 0u;
static uint32_t s_lastTx     = 0u;      /* 上一次发报告的時刻（保活用） */
static uint8_t  s_customStatus = 0xFFu; /* 上次上报的模式/链路状态字节 */
static uint32_t s_hidDeadMs = 0u;       /* HID 通道不可用的起始时刻（看门狗用） */
static uint32_t s_creditStallMs = 0u;   /* ACL 发送信用为 0 的起始时刻 */
static uint32_t s_cmdStallMs = 0u;      /* 未连接时 HCI 命令额度为 0 的起始时刻 */

/* 前置声明：bt_send_custom_report() 里要用，定义在文件后面 */
static uint8_t bt_status_byte(void);

static int32_t bt_mouse_read_axis(uint8_t axis)
{
    uint32_t sum = 0u;
    uint8_t i;

    for (i = 0u; i < BT_MOUSE_FILTER_SAMPLES; i++)
    {
        sum += Joystick_ReadAxis(axis);
    }
    return Joystick_MapRawValue(axis,
        (uint16_t)((sum + (BT_MOUSE_FILTER_SAMPLES / 2u)) /
                   BT_MOUSE_FILTER_SAMPLES));
}

static void bt_rotate_axes(int32_t lx, int32_t ly, int32_t *mx, int32_t *my)
{
    switch (BT_JOY_ROTATE)
    {
        case 1:  *mx =  ly; *my = -lx; break;
        case 2:  *mx = -lx; *my = -ly; break;
        case 3:  *mx = -ly; *my =  lx; break;
        default: *mx =  lx; *my =  ly; break;
    }
}

static int8_t bt_axis_to_move(int32_t v)
{
    return KMConfig_AxisToMove(v);   /* 与 USB 使用同一份运行配置和响应曲线 */
}

static uint8_t bt_read_key_bitmap(void)
{
    uint8_t bm = 0u;
    uint8_t k;

    for (k = KEY_A; k <= KEY_RG; k++)
    {
        if (Key_IsDown(k) != 0u)
        {
            bm |= (uint8_t)(1u << (k - KEY_A));
        }
    }
    return bm;
}

static void bt_send_keyboard_report(uint8_t bm, uint8_t modifier, uint8_t joy)
{
    s_lastTx = hal_time_ms();
    uint8_t rpt[10];
    uint8_t keys[6];
    uint8_t n = 0u;
    uint8_t base;
    uint8_t i;

    memset(rpt, 0, sizeof(rpt));
    (void)KMConfig_BuildKeyboard(bm, joy, keys);
    rpt[0] = 0xA1u;
    if (g_bt_hid_protocol == HIDP_PROTOCOL_BOOT)
    {
        base = 1u;
    }
    else
    {
        rpt[1] = BT_REPORT_KEYBOARD;
        base = 2u;
    }
    rpt[base] = modifier;
    for (i = 0u; i < 6u; ++i)
    {
        if (keys[i] != 0u) { rpt[base + 2u + n++] = keys[i]; }
    }
    bt_hid_send_report(rpt, (g_bt_hid_protocol == HIDP_PROTOCOL_BOOT) ? 9u : 10u);
}

static void bt_send_mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    s_lastTx = hal_time_ms();
    if (g_bt_hid_protocol == HIDP_PROTOCOL_BOOT)
    {
        uint8_t rpt[4] = { 0xA1u, buttons, (uint8_t)dx, (uint8_t)dy };
        bt_hid_send_report(rpt, 4u);
    }
    else
    {
        uint8_t rpt[6] = { 0xA1u, BT_REPORT_MOUSE, buttons,
                           (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel };
        bt_hid_send_report(rpt, 6u);
    }
}

static void bt_send_custom_report(uint8_t bm, uint8_t led, uint8_t joy)
{
    s_lastTx = hal_time_ms();
    uint8_t rpt[11];

    if (g_bt_hid_protocol != HIDP_PROTOCOL_REPORT)
    {
        return;                         /* 厂商自定义集合不属于 Boot 协议 */
    }
    memset(rpt, 0, sizeof(rpt));
    rpt[0] = 0xA1u;
    rpt[1] = BT_REPORT_CUSTOM;
    rpt[2] = bm;
    rpt[3] = s_customSeq;
    rpt[4] = led;
    rpt[5] = bt_status_byte();      /* rep[3]：工作模式 + 链路状态，供上位机显示 */
    rpt[10] = joy;                  /* rep[8]：bit0=L3/Shift，bit1=R3/Ctrl */
    bt_hid_send_report(rpt, 11u);   /* 0xA1 + Report ID 3 + 9 字节报告体 */
    s_customSeq++;
}

static void bt_reset_report_state(void)
{
    s_keyBm = 0u;
    s_keyModifier = 0u;
    s_mouseBtn = 0u;
    s_customBm = 0u;
    s_customJoy = 0u;
    s_customLed = 0xFFu;
    s_customBeat = 0u;
    s_btActive = 0u;
    s_protoDirty = 1u;
}

/* 切到 USB 前先在旧蓝牙链路上松开所有按键，防止主机一直收到保持状态。 */
static void bt_release_inputs(void)
{
    if (s_btActive != 0u)
    {
        bt_send_keyboard_report(0u, 0u, 0u);
        bt_send_mouse_report(0u, 0, 0, 0);
        bt_send_custom_report(0u, UsbHid_GetKeyboardLeds(), 0u);
    }
    bt_reset_report_state();
}

/* 供 usb_hid.c 组装自定义报告状态字节：蓝牙 HID 通道是否就绪 */
/* 诊断：最近一次断开原因码（0xFF = 本次上电还没断过） */
volatile uint8_t g_bt_last_disc_reason = 0xFFu;

/* 诊断：回连尝试次数（每发起一次 Create_Connection 加一） */
uint8_t BT_AppReconnectTries(void)
{
    return s_tryCount;
}

/* 诊断：当前蓝牙状态机状态（BT_STATE_xxx） */
uint8_t BT_AppState(void)
{
    return g_bt_state;
}

uint8_t BT_AppIsLinked(void)
{
    return (g_bt_state == BT_STATE_HID_READY) ? 1U : 0U;
}

/* 组装自定义报告里的"状态字节"（rep[3]）：
 *   bit0 = 1  当前是「强制蓝牙」模式
 *   bit1 = 1  蓝牙 HID 通道已就绪
 * 上位机据此显示当前工作模式。 */
static uint8_t bt_status_byte(void)
{
    uint8_t st = 0U;

    if (UsbHid_IsBtForced() != 0U)          /* bit0：当前输出走蓝牙 */
    {
        st |= 0x01U;
    }
    if (g_bt_state == BT_STATE_HID_READY)    /* bit1：蓝牙链路已就绪 */
    {
        st |= 0x02U;
    }
    if (UsbHid_IsBtPreferred() != 0U)        /* bit2：用户偏好"强制蓝牙" */
    {
        st |= 0x04U;
    }
    return st;
}

/* 长按切到蓝牙输出时调用：把链路状态机彻底复位后重新回连。
 * 从 USB 模式切回来时，控制器/协议栈可能残留着上一次连接的状态
 * （尤其是被看门狗判定死亡、控制器却没上报断开的情况），
 * 不复位就会卡在"永远回连不上"的脏状态里。 */
void BT_AppForceBtMode(void)
{
    /* 第一优先：链路本来就健康（HID 通道就绪）-> 直接切输出，绝不断开。
     * 之前无脑断开健康链路，结果重新回连失败，10 秒后被"放弃规则"退回 USB，
     * 用户看到的就是"蓝牙连着的时候切不过去"。 */
    if ((g_bt_state == BT_STATE_HID_READY) && (bt_hid_channel_ready() != 0u))
    {
        s_reconnectBusy = 0u;
        s_lastTry       = 0u;
        UART1_SendString("[bt] link healthy, keep it for BT output\r\n");
        return;
    }

    /* 链路状态不健康：断开残留链路并把状态机复位，重新回连 */
    if (g_conn_handle != 0u)
    {
        bt_release_inputs();
        bt_cmd_disconnect(g_conn_handle, 0x16u);
    }

    /* 强制清空链路状态，回到"可被发现/可回连"的干净起点 */
    g_conn_handle   = BT_HANDLE_NONE;
    g_bt_state      = BT_STATE_READY;
    bt_l2cap_reset();
    bt_app_on_disconnected();       /* 重置回连标志，并记录断开时刻 */
    s_reconnectBusy = 0u;
    /* 保留 bt_app_on_disconnected() 写入的 s_lastTry，等待 RECONNECT_MS 后再
     * 寻呼。稳定手柄工程依靠这段间隔让控制器释放旧 ACL；立即回连会被拒绝。 */
    s_hidCtrlMs     = 0u;
    s_creditStallMs = 0u;
    s_cmdStallMs    = 0u;
    s_hidDeadMs     = 0u;
    /* bt_app_on_disconnected() 已在蓝牙允许时恢复扫描，这里不能再发一次；
     * 控制器通常只有 1 个命令额度，重复发送会让后续回连永久等待。 */

    UART1_SendString("[bt] force BT mode: link state reset\r\n");
}

/* 切到蓝牙模式时调用：如果当前还挂着旧链路，先断开再让回连逻辑重建，
 * 保证拿到干净的 HID 控制/中断通道（否则可能出现"显示已连接但键鼠无反应"）。 */
void BT_AppRestartLink(void)
{
    if (g_conn_handle != 0u)
    {
        bt_release_inputs();
        bt_cmd_disconnect(g_conn_handle, 0x16u);
        UART1_SendString("[bt] link restart for clean HID channels\r\n");
    }
}

/* 长按 RG 切回「自动 / USB 优先」时由 usb_hid.c 调用：
 * 只松开蓝牙输入并切换选路，保持 HID 链路连接。反复断开/回连会在
 * 控制器尚未释放旧 ACL 时失败，保持链路后切回蓝牙可立即恢复输入。 */
void BT_AppForceUsbMode(void)
{
    bt_release_inputs();
    s_routeUsb = 1u;
    UART1_SendString("[mux] input -> USB; Bluetooth link kept alive\r\n");
}


void bt_app_set_keyboard_leds(uint8_t leds)
{
    UsbHid_SetKeyboardLeds(leds);
}

uint8_t bt_app_get_keyboard_leds(void)
{
    return UsbHid_GetKeyboardLeds();
}

void bt_app_on_output_report(const uint8_t *p, uint16_t len)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;

    if ((p == 0) || (len < 5u) || (p[0] != BT_REPORT_CUSTOM))
    {
        return;
    }
    if (p[1] == USB_HID_CMD_GET_STATUS)
    {
        s_customLed = 0xFFu;            /* 下一帧立即回传状态，不伪造按钮动作 */
        return;
    }
    if (p[1] == USB_HID_CMD_RAINBOW_START)
    {
        UsbHid_SetRainbowMode(1u);
        return;
    }
    if (p[1] == USB_HID_CMD_RAINBOW_STOP)
    {
        UsbHid_SetRainbowMode(0u);
        return;
    }
    if (p[1] != USB_HID_CMD_SET_RGB)
    {
        return;
    }
    UsbHid_SetRainbowMode(0u);
    r = (p[2] > 100u) ? 100u : p[2];
    g = (p[3] > 100u) ? 100u : p[3];
    b = (p[4] > 100u) ? 100u : p[4];
    Tim2_SetRedBrightness(r);
    Tim2_SetGreenBrightness(g);
    Tim2_SetBlueBrightness(b);
    UART1_Printf("[bt] rgb R=%u G=%u B=%u\r\n",
                 (unsigned)r, (unsigned)g, (unsigned)b);
}

void bt_app_on_protocol_changed(void)
{
    bt_reset_report_state();
}

static void bt_input_scan_and_send(uint32_t now)
{
    uint8_t bm = bt_read_key_bitmap();
    uint8_t modifier = 0u;
    uint8_t joy = 0u;
    uint8_t buttons = 0u;
    uint8_t led = UsbHid_GetKeyboardLeds();
    int32_t lx, ly, mx, my;
    int32_t rx, ry, wx, wy;
    int8_t dx;
    int8_t dy;
    int8_t wheel = 0;

    if (Joystick_KeyIsDown(JOYSTICK_L3) != 0u) { joy |= 0x01u; }
    if (Joystick_KeyIsDown(JOYSTICK_R3) != 0u) { joy |= 0x02u; }
    {
        uint8_t keys[6];
        modifier = KMConfig_BuildKeyboard(bm, joy, keys);
    }

    s_btActive = 1u;
    if ((bm != s_keyBm) || (modifier != s_keyModifier) || (s_protoDirty != 0u))
    {
        s_keyBm = bm;
        s_keyModifier = modifier;
        bt_send_keyboard_report(bm, modifier, joy);
    }

    buttons = KMConfig_BuildMouse(bm, joy);

    lx = bt_mouse_read_axis(JOYSTICK_LX);
    ly = bt_mouse_read_axis(JOYSTICK_LY);
    bt_rotate_axes(lx, ly, &mx, &my);
    dx = bt_axis_to_move(mx);
    dy = (int8_t)(-bt_axis_to_move(my));

    if ((uint32_t)(now - s_wheelMs) >= BT_WHEEL_INTERVAL)
    {
        s_wheelMs = now;
        rx = Joystick_MapRawValue(JOYSTICK_RX, Joystick_ReadAxis(JOYSTICK_RX));
        ry = Joystick_MapRawValue(JOYSTICK_RY, Joystick_ReadAxis(JOYSTICK_RY));
        bt_rotate_axes(rx, ry, &wx, &wy);
        (void)wx;
        wheel = KMConfig_Wheel(wy);
    }

    if ((dx != 0) || (dy != 0) || (wheel != 0) ||
        (buttons != s_mouseBtn) || (s_protoDirty != 0u))
    {
        s_mouseBtn = buttons;
        bt_send_mouse_report(buttons, dx, dy, wheel);
    }

    /* 自定义报告（report 3）只在内容变化时发。
     *
     * 【原来这里每秒无条件下发一帧，是"连上约 20 秒被主机断开(0x13)"的元凶】
     *   report 3 是厂商自定义集合，只有上位机打开它时才有人读。
     *   没人读的时候，Windows 的 HID 类驱动会把这些输入报告堆在队列里，
     *   堆满之后整台 HID 设备被判失败，BTHID 直接把蓝牙链路拆掉 ——
     *   断开原因正是 0x13（对端主动终止），周期正好是队列堆满的时间。 */
    if ((bm != s_customBm) || (joy != s_customJoy) || (led != s_customLed) ||
        (bt_status_byte() != s_customStatus) || (s_protoDirty != 0u))
    {
        bt_send_custom_report(bm, led, joy);
        s_customBm = bm;
        s_customJoy = joy;
        s_customLed = led;
        s_customStatus = bt_status_byte();
    }
    /* 【链路保活】与已验证稳定的手柄工程一致，每 20ms 都保证至少发出
     * 一帧有效输入报告。空闲时重复当前键盘状态，主机不会产生额外按键动作；
     * 使用 Windows 始终读取的键盘集合，不用无人读取时会堆积的自定义集合。 */
    if ((uint32_t)(now - s_lastTx) >= REPORT_MS)
    {
        bt_send_keyboard_report(s_keyBm, s_keyModifier, s_customJoy);
    }

    s_protoDirty = 0u;
}

uint8_t BT_Probe(void)
{
    typedef struct
    {
        uint32_t baud;
        uint32_t parity;
        uint32_t tag;         /* 统一用 32 位，结构体内部不会有填充字节 */
    } line_t;

    static const line_t lines[6] =
    {
        /* 实测 RDA5876 跑 921600 8N1，正常情况第一条就命中，
         * 后面几条是给别的固件版本留的兜底 */
        { 921600u, USART_Parity_No,   'N' },
        { 921600u, USART_Parity_Even, 'E' },
        { 921600u, USART_Parity_Odd,  'O' },
        { 115200u, USART_Parity_No,   'N' },
        { 115200u, USART_Parity_Even, 'E' },
        { 460800u, USART_Parity_No,   'N' }
    };

    uint8_t  cap[32];
    uint8_t  n;
    uint8_t  i;
    uint16_t k;
    uint16_t tries;

    for (i = 0u; i < 6u; i++)
    {
        BtUart_SetBaud(lines[i].baud, (uint16_t)lines[i].parity);
        bt_transport_reset();

        /* 同一条命令连发两次，中间隔 50ms：
         * 芯片刚启动时第一条可能被吃掉，发两次能显著提高命中率 */
        bt_send_cmd(BT_OP_Read_Local_Version, 0, 0u);
        Delay_ms(50);
        bt_send_cmd(BT_OP_Read_Local_Version, 0, 0u);

        n = 0u;
        for (k = 0u; k < 300u; k++)
        {
            uint8_t b;
            while ((n < (uint8_t)sizeof(cap)) && (BtUart_TryRead(&b) != 0u))
            {
                cap[n++] = b;
            }
            Delay_ms(1);
        }

        UART1_Printf("[probe] %lu %c rx=%u :", (unsigned long)lines[i].baud,
                     (int)lines[i].tag, (unsigned)n);
        for (tries = 0u; tries < (uint16_t)n; tries++)
        {
            UART1_Printf(" %02X", (unsigned)cap[tries]);
        }
        UART1_SendString("\r\n");

        /* 收到 04 0E（事件 + Command Complete）才算真通 */
        for (k = 0u; (uint16_t)(k + 1u) < (uint16_t)n; k++)
        {
            if ((cap[k] == H4_TYPE_EVENT) && (cap[k + 1u] == EVT_Command_Complete))
            {
                UART1_SendString("[probe] LINK ALIVE -> keep this setting\r\n");
                bt_transport_reset();
                return 1u;
            }
        }
    }

    BtUart_SetBaud(921600u, USART_Parity_Even);
    bt_transport_reset();
    UART1_SendString("[probe] DEAD on all 6 settings\r\n");
    return 0u;
}

void BT_Init(void)
{
    uint8_t ok;

    bt_l2cap_reset();

    if (Key_IsDown(KEY_A) != 0u)
    {
        UART1_SendString("[bond] KEY_A held at boot -> erase bond\r\n");
        bt_app_drop_bond();
    }

    bond_load();

    g_bt_state = BT_STATE_IDLE;

    UART1_SendString("[bt] init sequence start\r\n");
    ok = bt_init_sequence();

    if (ok == 0u)
    {
        g_bt_error = 1u;
        UART1_SendString("[bt] init FAILED: no event from controller\r\n");
        return;
    }

    g_bt_error = 0u;
    g_bt_state = BT_STATE_READY;
    s_reconnectBusy = 0u;
    s_scanOffPending = 0u;
    s_lastTry = 0u;
    s_tryCount = 0u;
    s_reconnectOn = (s_bondValid != 0u) ? 1u : 0u;

    UART1_Printf("[bt] ready, bond=%u (name=%s)\r\n",
                 (unsigned)s_bondValid, "MP KEYMOUSE");
}

static void print_status(uint32_t now)
{
    static uint32_t last = 0u;

    if ((uint32_t)(now - last) < 2000u)
    {
        return;
    }
    last = now;

    now = hal_time_ms();

    /* proto 只可能是 0(Boot) 或 1(Report)。出现 2 说明主机请求
     * 被误判成 SET_PROTOCOL —— 修复后不该再发生。 */
    UART1_Printf("[bt] st=%u ch=%u%u%u usb=%u proto=%u(%s) rpt=%lu t=%lu drop=%lu\r\n",
                 (unsigned)g_bt_state,
                 (unsigned)g_chan[BT_CHAN_SDP].open,
                 (unsigned)g_chan[BT_CHAN_HID_CTRL].open,
                 (unsigned)g_chan[BT_CHAN_HID_INT].open,
                 (unsigned)UsbHid_IsMounted(),
                 (unsigned)g_bt_hid_protocol,
                 (g_bt_hid_protocol == HIDP_PROTOCOL_BOOT) ? "Boot" : "Report",
                 (unsigned long)g_bt_reports,
                 (unsigned long)(now - s_connMs),
                 (unsigned long)BtUart_RxDropped());
}


void BT_Task(void)
{
    uint32_t now = hal_time_ms();

    /* 1. 收包（必须每轮跑，不能有毫秒级阻塞） */
    bt_transport_poll();

    /* 1.5 连上之后周期报一次通道状态，方便定位卡在哪个通道 */
    if (g_conn_handle != BT_HANDLE_NONE)
    {
        print_status(now);
    }

    if ((s_bondPending != 0u) && (g_conn_handle != BT_HANDLE_NONE) &&
        (bt_hid_channel_ready() != 0u))
    {
        if (s_bondStamp == 0u)
        {
            s_bondStamp = now;
        }
        else if ((uint32_t)(now - s_bondStamp) > BOND_DELAY_MS)
        {
            bt_app_commit_bond();
        }
    }

    /* 3. 已连接：按 20ms 采样并发送键鼠报告 */
    if (g_conn_handle != BT_HANDLE_NONE)
    {
        if ((g_chan[BT_CHAN_HID_CTRL].open != 0u) &&
            (g_chan[BT_CHAN_HID_INT].used == 0u))
        {
            if (s_hidCtrlMs == 0u)
            {
                s_hidCtrlMs = now;
            }
            else if ((uint32_t)(now - s_hidCtrlMs) > 300u)
            {
                bt_l2cap_open_hid_interrupt();
            }
        }

        if (bt_hid_channel_ready() != 0u)
        {
            if (g_bt_state != BT_STATE_HID_READY)
            {
                g_bt_state = BT_STATE_HID_READY;

                /* 通道刚就绪：先发一帧"全部松开"，清掉主机侧可能残留的
                 * 按键/按钮状态（上一轮断开时如果按键正按着，主机可能
                 * 一直认为它还按着，表现为"按键后键鼠都不正常"）。 */
                bt_send_keyboard_report(0u, 0u, 0u);
                bt_send_mouse_report(0u, 0, 0, 0);
                bt_send_custom_report(0u, UsbHid_GetKeyboardLeds(), 0u);

                /* 独立供电时 s_btEff 在蓝牙连接前就已经是 1，模式切换处的
                 * 校准会早于射频链路建立。HID 通道就绪后再校准一次，才能消除
                 * 蓝牙正式工作后产生的摇杆中位偏移，避免光标静止时向上漂。 */
                Joystick_RecalibrateNow();
                bt_reset_report_state();
                s_routeUsb = 0xFFu;
                UART1_SendString("[bt] HID interrupt channel open\r\n");
            }

            if ((UsbHid_IsMounted() != 0u) && (UsbHid_IsBtPreferred() == 0u))
            {
                if (s_routeUsb != 1u)
                {
                    s_routeUsb = 1u;

                    /* USB 输出期间只松开蓝牙输入，不再断开 ACL/HID 链路。
                     * 后续空键盘报告负责保活，主机不会收到任何按键或移动。 */
                    bt_release_inputs();
                    UART1_SendString("[mux] USB in use -> Bluetooth input paused\r\n");
                }

                /* 使用 Windows 始终读取的键盘集合保活；不能周期发自定义报告，
                 * 否则上位机未打开自定义集合时可能在主机侧堆积输入报告。 */
                if ((uint32_t)(now - s_lastTx) >= REPORT_MS)
                {
                    bt_send_keyboard_report(0u, 0u, 0u);
                }
            }
            else
            {
                if (s_routeUsb != 0u)
                {
                    bt_reset_report_state();
                    s_routeUsb = 0u;
                    UART1_SendString("[mux] input -> BLUETOOTH\r\n");
                }
                if ((uint32_t)(now - s_lastSample) >= SAMPLE_MS)
                {
                    s_lastSample = now;
                    if ((uint32_t)(now - s_lastReport) >= REPORT_MS)
                    {
                        bt_input_scan_and_send(now);
                        s_lastReport = now;
                    }
                }
            }
        }
        else if (g_bt_state == BT_STATE_HID_READY)
        {
            g_bt_state = BT_STATE_CONNECTED;
            bt_reset_report_state();
            s_routeUsb = 0xFFu;
        }
    }
    else
    {
        /* 控制器异常没有回事件时也不能永久卡死；恢复扫描后再试。 */
        if ((s_reconnectBusy != 0u) &&
            ((uint32_t)(now - s_lastTry) >= 20000u))
        {
            s_reconnectBusy = 0u;
            s_scanOffPending = 0u;
            g_conn_by_us = 0u;
            s_lastTry = now;
            bt_cmd_scan_enable(0x03u);
            UART1_SendString("[bt] reconnect event timeout; scan restored\r\n");
        }

        /* 3.5 HID 通道看门狗：
         * 状态机说"HID 就绪"，但中断通道实际不可用（报告发不出去），
         * 且持续超过 2 秒 -> 认为链路已残废，主动断开让回连逻辑重建。
         * 否则会出现"显示已连接、键鼠全无反应、主机也收不到任何报告"。 */
        if ((g_bt_state == BT_STATE_HID_READY) && (bt_hid_channel_ready() == 0u))
        {
            if (s_hidDeadMs == 0u)
            {
                s_hidDeadMs = now;
            }
            else if ((uint32_t)(now - s_hidDeadMs) >= 2000u)
            {
                s_hidDeadMs = 0u;
                UART1_SendString("[bt] HID channel dead >2s, restarting link\r\n");
                bt_release_inputs();
                if (g_conn_handle != 0u)
                {
                    bt_cmd_disconnect(g_conn_handle, 0x16u);
                }
            }
        }
        else
        {
            s_hidDeadMs = 0u;
        }

        /* 4.0 回连连续失败太多次 -> 重新初始化蓝牙控制器。
         * 每次模式切换都要断开/重连，反复几十次之后 RDA5876 内部状态
         * 可能累积错误，表现为"再也连不上蓝牙"。这里重新走一遍
         * 上电时序 + 厂商补丁 + 18 条初始化命令（约 1.5 秒），
         * 配对信息保存在 Flash 里不会丢，初始化后即可正常回连。 */
        if ((s_reconnectBusy == 0u) && (s_tryCount >= 6u) &&
            (s_reconnectOn != 0u) && (s_bondValid != 0u) &&
            ((UsbHid_IsMounted() == 0u) || (UsbHid_IsBtPreferred() != 0u)))
        {
            s_tryCount = 0u;
            UART1_SendString("[bt] reconnect failed 6 times -> reinit controller\r\n");

            g_conn_handle    = BT_HANDLE_NONE;
            g_bt_state       = BT_STATE_IDLE;
            bt_l2cap_reset();

            if (Init_rda5876() == RDA_OK)
            {
                if (BT_Probe() != 0u)
                {
                    BT_Init();
                }
            }

            s_reconnectBusy  = 0u;
            s_scanOffPending = 0u;
            s_lastTry        = 0u;
            s_hidCtrlMs      = 0u;
            s_reconnectOn    = (s_bondValid != 0u) ? 1u : 0u;
            if (g_bt_error == 0u)
            {
                g_bt_state = BT_STATE_READY;
            }
        }

        /* 4. 未连接：只要配对仍有效就持续回连。
         * 先关扫描，等 Command Complete 归还命令额度后再创建连接。 */
        if ((s_reconnectBusy == 0u) && (s_reconnectOn != 0u) &&
            (s_bondValid != 0u) &&
            ((UsbHid_IsMounted() == 0u) || (UsbHid_IsBtPreferred() != 0u)))
        {
            /* USB 正在用的时候不回连：等拔掉 USB 再恢复自动回连 */
            if (((s_lastTry == 0u) ||
                 ((uint32_t)(now - s_lastTry) >= RECONNECT_MS)) &&
                (g_cmd_credits > 0u))
            {
                s_lastTry = now;
                s_tryCount++;
                s_reconnectBusy = 1u;
                s_scanOffPending = 1u;

                UART1_Printf("[bt] reconnect try %u: disable scan first\r\n",
                             (unsigned)s_tryCount);
                bt_cmd_scan_enable(0x00u);
            }
        }

        /* 断开事件已经把 s_reconnectBusy 清零，但若前一条命令的完成事件
         * 恰好丢失，g_cmd_credits 会永远停在 0，上面的回连条件再也无法成立。
         * 未连接且空闲超过 2 秒仍无额度时恢复一个额度，但重试仍等待正常
         * 回连间隔；有连接或正在建链时不恢复，避免干扰正常 HCI 流控。 */
        if ((s_reconnectBusy == 0u) && (s_reconnectOn != 0u) &&
            (s_bondValid != 0u) && (g_cmd_credits == 0u) &&
            ((UsbHid_IsMounted() == 0u) || (UsbHid_IsBtPreferred() != 0u)))
        {
            if (s_cmdStallMs == 0u)
            {
                s_cmdStallMs = now;
            }
            else if ((uint32_t)(now - s_cmdStallMs) >= 2000u)
            {
                s_cmdStallMs = 0u;
                g_cmd_credits = 1u;
                s_lastTry = now;    /* 恢复正常回连间隔，不立即寻呼 */
                UART1_SendString("[bt] HCI command credit timeout; retry reconnect\r\n");
            }
        }
        else
        {
            s_cmdStallMs = 0u;
        }

        if (g_bt_state != BT_STATE_READY && g_bt_error == 0u)
        {
            g_bt_state = BT_STATE_READY;
        }

        if (g_acl_credits == 0u)
        {
            g_acl_credits = g_acl_pkts;
        }
    }

    /* 3.6 ACL 信用停滞看门狗（判定"链路已死"最可靠的迹象）
     * 正常时控制器用 "Number Of Completed Packets" 事件归还发送信用，
     * 4 个信用几毫秒就回来了。如果连续 3 秒信用一直是 0，说明控制器
     * 已经不再发送 ACL —— 典型场景就是电脑端把蓝牙关了，但控制器
     * 迟迟不报 Disconnection Complete。此时所有 HID 报告都会静默失败，
     * 用户看到的就是"显示已连接、键鼠完全没反应"。
     * 此检查必须同时覆盖有、无连接句柄两种情况，否则残留句柄会挡住看门狗。 */
    if ((g_bt_state == BT_STATE_HID_READY) && (g_acl_credits == 0u))
    {
        if (s_creditStallMs == 0u)
        {
            s_creditStallMs = now;
        }
        else if ((uint32_t)(now - s_creditStallMs) >= 3000u)
        {
            s_creditStallMs = 0u;
            UART1_SendString("[bt] ACL credits stalled >3s, link presumed dead\r\n");

            /* ACL 已连续 3 秒无法发送，链路本身已不可用。这里只清理本地
             * 失效状态，不再叠加一条 Disconnect；否则它和下一条 Scan Enable
             * 会同时占用唯一的 HCI 命令额度，反而让回连状态机卡死。 */
            g_conn_handle = BT_HANDLE_NONE;
            g_bt_state = BT_STATE_READY;
            bt_l2cap_reset();
            bt_app_on_disconnected();
            g_bt_last_disc_reason = 0x08u;                 /* 诊断：标为超时 */
        }
    }
    else
    {
        s_creditStallMs = 0u;
    }
}
