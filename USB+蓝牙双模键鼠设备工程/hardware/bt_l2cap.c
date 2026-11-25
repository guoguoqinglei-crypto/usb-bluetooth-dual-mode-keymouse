#include <bt_hci.h>
#include <bt_sdp_record.h>

#include <string.h>
#include <stm32f10x.h>

#include <uart.h>
#include <delay.h>

#define L2CAP_MTU_LOCAL   672u

#define BT_TRACE_ACL   0

#define BT_TRACE_SIG   1

/* ACL 重组缓冲 */
static uint8_t  s_aclBuf[BT_ACL_BUF_SIZE];
static uint16_t s_aclLen   = 0u;    /* 已拼字节数 */
static uint16_t s_aclTotal = 0u;    /* 本 PDU 总长（L2CAP 头 + 负载） */

static uint16_t s_nextCid = 0x0040u;

static uint8_t  s_sigId = 0x10u;

uint8_t g_bt_hid_protocol = HIDP_PROTOCOL_REPORT;

static uint8_t hidp_handle_output_led(const uint8_t *p, uint16_t len)
{
    if (g_bt_hid_protocol == HIDP_PROTOCOL_REPORT)
    {
        if (len < 3u)
        {
            return HIDP_HS_ERR_PARAMETER;
        }
        if (p[1] == 0x01u)
        {
            bt_app_set_keyboard_leds(p[2]);
            return HIDP_HS_SUCCESS;
        }
        if (p[1] == 0x03u)
        {
            /* 手柄工程的控制通道输出处理保留，报告内容换成键鼠 ID3。 */
            bt_app_on_output_report(&p[1], (uint16_t)(len - 1u));
            return HIDP_HS_SUCCESS;
        }
        return HIDP_HS_ERR_REPORT_ID;
    }

    if (len < 2u)
    {
        return HIDP_HS_ERR_PARAMETER;
    }
    bt_app_set_keyboard_leds(p[1]);      /* Boot 输出只属于键盘 */
    return HIDP_HS_SUCCESS;
}

static uint8_t sig_next_id(void)
{
    uint8_t v = s_sigId++;
    if (s_sigId == 0u)
    {
        s_sigId = 0x10u;
    }
    return v;
}

/* 发送缓冲：SDP 响应 653 字节 + L2CAP 头 */
static uint8_t  s_out[700];

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)(v >> 8);
}

static void wr16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

void bt_l2cap_reset(void)
{
    uint8_t i;

    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        g_chan[i].psm     = 0u;
        g_chan[i].lcid    = 0u;
        g_chan[i].rcid    = 0u;
        g_chan[i].used    = 0u;
        g_chan[i].cfg_sent= 0u;
        g_chan[i].cfg_rcvd= 0u;
        g_chan[i].open    = 0u;
    }

    g_chan[BT_CHAN_SDP].psm       = L2CAP_PSM_SDP;
    g_chan[BT_CHAN_HID_CTRL].psm  = L2CAP_PSM_HID_CONTROL;
    g_chan[BT_CHAN_HID_INT].psm   = L2CAP_PSM_HID_INTERRUPT;

    s_aclLen   = 0u;
    s_aclTotal = 0u;
    s_nextCid  = 0x0040u;

    /* 每条新链路都回到默认 Report 协议。上一版断线后不清这个变量，
     * 一旦被误写就带着错误格式活到下一次上电。 */
    g_bt_hid_protocol = HIDP_PROTOCOL_REPORT;
}

static uint16_t cid_alloc(void)
{
    uint16_t c = s_nextCid++;
    if (s_nextCid == 0x0000u || s_nextCid > 0x00FFu)
    {
        s_nextCid = 0x0040u;            /* 回绕保护，够用就行 */
    }
    return c;
}

static int ch_by_psm(uint16_t psm)
{
    uint8_t i;
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if (g_chan[i].psm == psm)
        {
            return (int)i;
        }
    }
    return -1;
}

static int ch_by_lcid(uint16_t lcid)
{
    uint8_t i;
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if ((g_chan[i].used != 0u) && (g_chan[i].lcid == lcid))
        {
            return (int)i;
        }
    }
    return -1;
}

static int ch_by_rcid(uint16_t rcid)
{
    uint8_t i;
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if ((g_chan[i].used != 0u) && (g_chan[i].rcid == rcid))
        {
            return (int)i;
        }
    }
    /* 有些主机在 CONF_RSP 里放的是本端 CID，两种都认，容错 */
    for (i = 0u; i < BT_CHAN_COUNT; i++)
    {
        if ((g_chan[i].used != 0u) && (g_chan[i].lcid == rcid))
        {
            return (int)i;
        }
    }
    return -1;
}

static void ch_clear(int idx)
{
    if (idx < 0)
    {
        return;
    }
    g_chan[idx].lcid     = 0u;
    g_chan[idx].rcid     = 0u;
    g_chan[idx].used     = 0u;
    g_chan[idx].cfg_sent = 0u;
    g_chan[idx].cfg_rcvd = 0u;
    g_chan[idx].open     = 0u;
}

static void l2cap_send(uint16_t cid, const uint8_t *payload, uint16_t len)
{
    /* 超过单个 ACL 包上限时由 bt_send_acl() 自动分片（PB=10/01），
     * 这里只拦"连分片缓冲都放不下"的异常长度。 */
    if (len > BT_TX_ACL_MAX)
    {
        UART1_Printf("[l2] drop: len=%u > %u\r\n",
                     (unsigned)len, (unsigned)BT_TX_ACL_MAX);
        return;
    }
    bt_send_acl(g_conn_handle, cid, payload, len);
}

static void sig_send(uint8_t code, uint8_t id,
                     const uint8_t *data, uint16_t len)
{
    uint8_t  buf[64];
    uint16_t total = (uint16_t)(4u + len);

    if (total > (uint16_t)sizeof(buf))
    {
        return;
    }

    buf[0] = code;
    buf[1] = id;
    wr16(&buf[2], len);
    if (len != 0u)
    {
        memcpy(&buf[4], data, len);
    }

    /* 把发出去的信令也打出来，和收到的对照才知道响应对不对 */
#if BT_TRACE_SIG
    {
        uint16_t n = (len > 10u) ? 10u : len;
        uint16_t j;

        UART1_Printf("[l2] TX code=0x%02X id=%u len=%u :",
                     (unsigned)code, (unsigned)id, (unsigned)len);
        for (j = 0u; j < n; j++)
        {
            UART1_Printf(" %02X", (unsigned)data[j]);
        }
        UART1_SendString("\r\n");
    }
#endif

    l2cap_send(L2CAP_CID_SIGNALING, buf, total);
}

static void sdp_send_attr_rsp(uint16_t tid, uint16_t rcid)
{
    uint16_t pos = 0u;
    uint16_t attrCount;
    uint16_t paramLen;

    attrCount = (uint16_t)(3u + (uint16_t)sizeof(bt_sdp_attribute_list));
    paramLen  = (uint16_t)(2u + attrCount + 1u);

    s_out[pos++] = SDP_PDU_SERVICE_SEARCH_ATTR_RSP;   /* 0x07 */
    wr16be(&s_out[pos], tid);  pos = (uint16_t)(pos + 2u);
    wr16be(&s_out[pos], paramLen); pos = (uint16_t)(pos + 2u);
    wr16be(&s_out[pos], attrCount); pos = (uint16_t)(pos + 2u);

    /* 外层 SEQUENCE：0x36 + 16 位长度(sizeof) */
    s_out[pos++] = 0x36u;
    s_out[pos++] = (uint8_t)(sizeof(bt_sdp_attribute_list) >> 8);
    s_out[pos++] = (uint8_t)(sizeof(bt_sdp_attribute_list) & 0xFFu);

    memcpy(&s_out[pos], bt_sdp_attribute_list,
           sizeof(bt_sdp_attribute_list));
    pos = (uint16_t)(pos + sizeof(bt_sdp_attribute_list));

    s_out[pos++] = 0x00u;            /* ContinuationState = 0（没有后续段） */

    l2cap_send(rcid, s_out, pos);

    UART1_Printf("[sdp] attr rsp %u bytes -> cid=0x%04X\r\n",
                 (unsigned)pos, (unsigned)rcid);
}

/* SDP 请求的处理直接放在 bt_acl_input() 里做（数据量小、同步回即可），
 * 这里只保留响应组包。 */

static void hidp_control_rsp(uint16_t cid, const uint8_t *p, uint16_t len)
{
    uint8_t hdr;
    uint8_t txn;
    uint8_t param;
    uint8_t hs;          /* HANDSHAKE 应答码 */

    if (len < 1u)
    {
        return;
    }

    hdr   = p[0];
    txn   = (uint8_t)((hdr >> 4) & 0x0Fu);
    param = (uint8_t)(hdr & 0x0Fu);

    switch (txn)
    {
        case HIDP_TRANS_GET_REPORT:
        {
            /* 参数低 2 位 = 报告类型，bit3 = Buffer Size 标志。
             * 带 Size 标志时后面有 2 字节缓冲大小，要先跳过再取 Report ID。 */
            uint8_t  rtype = (uint8_t)(param & 0x03u);
            uint16_t pos   = 1u;
            uint8_t  rid   = 0x01u;

            if ((param & HIDP_RPT_SIZE_FLAG) != 0u)
            {
                pos = (uint16_t)(pos + 2u);
            }
            if ((g_bt_hid_protocol == HIDP_PROTOCOL_REPORT) &&
                (len > pos) && (p[pos] != 0x00u))
            {
                rid = p[pos];
            }

            if (rtype == HIDP_RPT_TYPE_INPUT)
            {
                /* 回一份"无按键、无位移"的 DATA 报告，格式跟当前协议走 */
                if (g_bt_hid_protocol == HIDP_PROTOCOL_REPORT)
                {
                    if (rid == 0x02u)
                    {
                        uint8_t rpt[6] = { 0xA1u, 0x02u, 0u, 0u, 0u, 0u };
                        l2cap_send(cid, rpt, 6u);
                    }
                    else if (rid == 0x03u)
                    {
                        uint8_t rpt[10] = { 0xA1u, 0x03u, 0u, 0u, 0u,
                                            0u, 0u, 0u, 0u, 0u };
                        l2cap_send(cid, rpt, 10u);
                    }
                    else
                    {
                        uint8_t rpt[10] = { 0xA1u, 0x01u, 0u, 0u, 0u, 0u,
                                            0u, 0u, 0u, 0u };
                        l2cap_send(cid, rpt, 10u);
                    }
                }
                else
                {
                    if (rid == 0x02u)
                    {
                        uint8_t rpt[4] = { 0xA1u, 0u, 0u, 0u };   /* 按键 / X / Y */
                        l2cap_send(cid, rpt, 4u);
                    }
                    else
                    {
                        uint8_t rpt[9] = { 0xA1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
                        l2cap_send(cid, rpt, 9u);
                    }
                }

                UART1_Printf("[hid] GET_REPORT(input) rid=%u proto=%s -> DATA\r\n",
                             (unsigned)rid,
                             (g_bt_hid_protocol == HIDP_PROTOCOL_BOOT) ? "Boot" : "Report");
                return;
            }

            if (rtype == HIDP_RPT_TYPE_OUTPUT)
            {
                /* 键盘 LED 输出报告：把当前 LED 状态回给主机 */
                uint8_t led = bt_app_get_keyboard_leds();

                if (g_bt_hid_protocol == HIDP_PROTOCOL_REPORT)
                {
                    uint8_t rpt[3] = { 0xA2u, 0x01u, led };
                    l2cap_send(cid, rpt, 3u);
                }
                else
                {
                    uint8_t rpt[2] = { 0xA2u, led };
                    l2cap_send(cid, rpt, 2u);
                }

                UART1_Printf("[hid] GET_REPORT(output) -> DATA led=0x%02X\r\n",
                             (unsigned)led);
                return;
            }

            /* 其它报告类型（Feature 等）不支持 */
            hs = HIDP_HS_ERR_UNSUPPORTED;
            break;
        }

        case HIDP_TRANS_SET_REPORT:
        {
            uint8_t rtype = (uint8_t)(param & 0x03u);

            if (rtype == HIDP_RPT_TYPE_OUTPUT)
            {
                hs = hidp_handle_output_led(p, len);
                UART1_Printf("[hid] SET_REPORT(output) -> led update, hs=0x%02X\r\n",
                             (unsigned)hs);
            }
            else
            {
                hs = HIDP_HS_ERR_UNSUPPORTED;
            }
            break;
        }

        case HIDP_TRANS_GET_PROTOCOL:
        {
            /* 响应是 DATA：0xA0 + 协议值，两字节。
             * 注意 0xA0 是"DATA 头"，不是 Report/Boot 的取值。 */
            uint8_t rsp[2] = { 0xA0u, g_bt_hid_protocol };
            l2cap_send(cid, rsp, 2u);
            UART1_Printf("[hid] GET_PROTOCOL -> DATA %s\r\n",
                         (g_bt_hid_protocol == HIDP_PROTOCOL_BOOT) ? "Boot" : "Report");
            return;
        }

        case HIDP_TRANS_SET_PROTOCOL:
        {
            /* 参数：0 = Boot，1 = Report，别的值都是非法参数 */
            if (param > HIDP_PROTOCOL_REPORT)
            {
                hs = HIDP_HS_ERR_PARAMETER;
            }
            else if (param != g_bt_hid_protocol)
            {
                g_bt_hid_protocol = param;
                bt_app_on_protocol_changed();   /* 缓存作废，按新格式重发状态 */
                hs = HIDP_HS_SUCCESS;
            }
            else
            {
                hs = HIDP_HS_SUCCESS;
            }
            UART1_Printf("[hid] SET_PROTOCOL(%u) -> %s, hs=0x%02X\r\n",
                         (unsigned)param,
                         (param == HIDP_PROTOCOL_BOOT) ? "Boot" : "Report",
                         (unsigned)hs);
            break;
        }

        case HIDP_TRANS_GET_IDLE:
        {
            /* DATA：0xA0 + idle rate（0 = 只在变化时上报） */
            uint8_t idle[2] = { 0xA0u, 0x00u };
            l2cap_send(cid, idle, 2u);
            UART1_SendString("[hid] GET_IDLE -> DATA idle=0\r\n");
            return;
        }

        case HIDP_TRANS_SET_IDLE:
            /* 参数低 4 位是 idle rate 的高 4 位，后面可跟完整字节；一律接受 */
            hs = HIDP_HS_SUCCESS;
            UART1_Printf("[hid] SET_IDLE param=%u -> OK\r\n", (unsigned)param);
            break;

        case HIDP_TRANS_HID_CONTROL:
            /* 挂起/恢复控制，直接握手成功 */
            hs = HIDP_HS_SUCCESS;
            break;

        default:
            /* 保留事务号（含 0x2/0x3）：不支持，不是"参数错" */
            hs = HIDP_HS_ERR_UNSUPPORTED;
            break;
    }

    l2cap_send(cid, &hs, 1u);
    UART1_Printf("[hid] CTRL hdr=0x%02X txn=0x%X -> HANDSHAKE 0x%02X\r\n",
                 (unsigned)hdr, (unsigned)txn, (unsigned)hs);
}

static void sig_conn_req(const uint8_t *p, uint8_t id)
{
    uint16_t psm  = rd16(&p[0]);
    uint16_t scid = rd16(&p[2]);
    int      idx  = ch_by_psm(psm);
    uint8_t  rsp[8];

    if (idx < 0)
    {
        wr16(&rsp[0], scid);
        wr16(&rsp[2], 0u);
        wr16(&rsp[4], L2CAP_CONN_PSM_NOT_SUPPORT);
        wr16(&rsp[6], 0u);
        sig_send(L2CAP_SIG_CONN_RSP, id, rsp, 8u);
        UART1_Printf("[l2] conn_req psm=0x%04X -> NOT SUPPORTED\r\n", (unsigned)psm);
        return;
    }

    if (g_chan[idx].open != 0u)
    {
        wr16(&rsp[0], scid);
        wr16(&rsp[2], 0u);
        wr16(&rsp[4], L2CAP_CONN_NO_RESOURCES);
        wr16(&rsp[6], 0u);
        sig_send(L2CAP_SIG_CONN_RSP, id, rsp, 8u);
        UART1_Printf("[l2] conn_req psm=0x%04X -> BUSY\r\n", (unsigned)psm);
        return;
    }

    /* 之前失败遗留的脏槽允许被重新使用 */
    if (g_chan[idx].lcid == 0u)
    {
        g_chan[idx].lcid = cid_alloc();
    }
    g_chan[idx].rcid     = scid;
    g_chan[idx].used     = 1u;
    g_chan[idx].cfg_sent = 0u;
    g_chan[idx].cfg_rcvd = 0u;
    g_chan[idx].open     = 0u;

    wr16(&rsp[0], scid);
    wr16(&rsp[2], g_chan[idx].lcid);
    wr16(&rsp[4], L2CAP_CONN_SUCCESS);
    wr16(&rsp[6], 0u);
    sig_send(L2CAP_SIG_CONN_RSP, id, rsp, 8u);

    UART1_Printf("[l2] channel OPEN psm=0x%04X lcid=0x%04X rcid=0x%04X\r\n",
                 (unsigned)psm, (unsigned)g_chan[idx].lcid, (unsigned)scid);

    {
        uint8_t cfg[8];
        wr16(&cfg[0], scid);            /* DCID = 对方 CID */
        wr16(&cfg[2], 0x0000u);         /* Flags */
        cfg[4] = 0x01u;                 /* 选项类型：MTU */
        cfg[5] = 0x02u;                 /* 选项长度 */
        wr16(&cfg[6], L2CAP_MTU_LOCAL);
        sig_send(L2CAP_SIG_CONF_REQ, sig_next_id(), cfg, 8u);
        g_chan[idx].cfg_sent = 1u;
    }
}

static void sig_conn_rsp(const uint8_t *p)
{
    uint16_t dcid   = rd16(&p[0]);
    uint16_t scid   = rd16(&p[2]);
    uint16_t result = rd16(&p[4]);
    int      idx    = ch_by_lcid(dcid);

    if (idx < 0)
    {
        return;
    }

    if (result == L2CAP_CONN_PENDING)
    {
        UART1_Printf("[l2] our CONN_REQ pending psm=0x%04X, waiting\r\n",
                     (unsigned)g_chan[idx].psm);
        return;
    }

    if (result != L2CAP_CONN_SUCCESS)
    {
        UART1_Printf("[l2] our CONN_REQ rejected psm=0x%04X result=%u\r\n",
                     (unsigned)g_chan[idx].psm, (unsigned)result);
        ch_clear(idx);
        return;
    }

    g_chan[idx].rcid = scid;
    g_chan[idx].used = 1u;
    UART1_Printf("[l2] our CONN_REQ accepted psm=0x%04X lcid=0x%04X rcid=0x%04X\r\n",
                 (unsigned)g_chan[idx].psm, (unsigned)g_chan[idx].lcid, (unsigned)scid);

    {
        uint8_t cfg[8];
        wr16(&cfg[0], scid);
        wr16(&cfg[2], 0x0000u);
        cfg[4] = 0x01u;
        cfg[5] = 0x02u;
        wr16(&cfg[6], L2CAP_MTU_LOCAL);
        sig_send(L2CAP_SIG_CONF_REQ, sig_next_id(), cfg, 8u);
        g_chan[idx].cfg_sent = 1u;
    }
}

static void sig_conf_req(const uint8_t *p, uint16_t len, uint8_t id)
{
    uint16_t dcid = rd16(&p[0]);
    int      idx  = ch_by_lcid(dcid);
    uint8_t  rsp[12];
    uint16_t pos;
    uint16_t off;
    uint8_t  hasMtu = 0u;

    if (idx < 0)
    {
        return;
    }

    off = 4u;
    while ((uint16_t)(off + 2u) <= len)
    {
        uint8_t type = p[off];
        uint8_t olen = p[off + 1u];

        if ((uint16_t)(off + 2u + olen) > len)
        {
            break;
        }
        if ((type == 0x01u) && (olen == 2u))
        {
            hasMtu = 1u;
        }
        off = (uint16_t)(off + 2u + olen);
    }

    wr16(&rsp[0], g_chan[idx].lcid);
    wr16(&rsp[2], 0x0000u);
    wr16(&rsp[4], L2CAP_CONF_SUCCESS);
    pos = 6u;

    if (hasMtu != 0u)
    {
        rsp[pos++] = 0x01u;
        rsp[pos++] = 0x02u;
        wr16(&rsp[pos], L2CAP_MTU_LOCAL);
        pos = (uint16_t)(pos + 2u);
    }

    sig_send(L2CAP_SIG_CONF_RSP, id, rsp, pos);

    g_chan[idx].cfg_rcvd = 1u;
    if (g_chan[idx].cfg_sent != 0u)
    {
        g_chan[idx].open = 1u;
        UART1_Printf("[l2] channel CONFIGURED psm=0x%04X\r\n",
                     (unsigned)g_chan[idx].psm);
    }
}

static void sig_conf_rsp(const uint8_t *p, uint16_t len)
{
    uint16_t scid   = rd16(&p[0]);
    uint16_t result = (len >= 6u) ? rd16(&p[4]) : 0u;
    int      idx    = ch_by_rcid(scid);

    if (idx < 0)
    {
        return;
    }

    if (result != L2CAP_CONF_SUCCESS)
    {
        /* 配置失败：把槽位彻底放掉，允许主机重新发起 */
        ch_clear(idx);
        UART1_Printf("[l2] conf FAIL result=0x%04X, slot released\r\n",
                     (unsigned)result);
        return;
    }

    g_chan[idx].cfg_sent = 1u;   /* 已确认 */
    if (g_chan[idx].cfg_rcvd != 0u)
    {
        g_chan[idx].open = 1u;
        UART1_Printf("[l2] channel CONFIGURED psm=0x%04X\r\n",
                     (unsigned)g_chan[idx].psm);
    }
}

static void sig_disc_req(const uint8_t *p, uint8_t id)
{
    uint16_t dcid = rd16(&p[0]);
    uint16_t scid = rd16(&p[2]);
    uint8_t  rsp[4];
    int      idx  = ch_by_lcid(dcid);

    wr16(&rsp[0], dcid);
    wr16(&rsp[2], scid);
    sig_send(L2CAP_SIG_DISC_RSP, id, rsp, 4u);

    if (idx >= 0)
    {
        UART1_Printf("[l2] channel CLOSED psm=0x%04X\r\n",
                     (unsigned)g_chan[idx].psm);
        ch_clear(idx);

        /* HID 通道掉了就退回"已连接未就绪" */
        if (bt_hid_channel_ready() == 0u)
        {
            if (g_bt_state == BT_STATE_HID_READY)
            {
                g_bt_state = BT_STATE_CONNECTED;
            }
        }
    }
}

static void sig_disc_rsp(const uint8_t *p)
{
    uint16_t dcid = rd16(&p[0]);
    int      idx  = ch_by_lcid(dcid);

    if (idx >= 0)
    {
        ch_clear(idx);
    }
}

static void sig_info_req(const uint8_t *p, uint8_t id)
{
    uint16_t type = rd16(&p[0]);
    uint8_t  rsp[4];

    wr16(&rsp[0], type);       /* 回显 InfoType */
    wr16(&rsp[2], 0x0001u);    /* Result = 1：不支持的 InfoType */
    sig_send(L2CAP_SIG_INFO_RSP, id, rsp, 4u);
}

void bt_acl_input(const uint8_t *data, uint16_t len)
{
    uint16_t flags;
    uint8_t  pb;
    uint16_t dlen;
    uint16_t l2len;

    if (len < 4u)
    {
        return;
    }

    flags = (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
    pb    = (uint8_t)((flags >> 12) & 0x03u);
    dlen  = rd16(&data[2]);

#if BT_TRACE_ACL
    UART1_Printf("[acl] h=%04X pb=%u len=%u\r\n",
                 (unsigned)(flags & 0x0FFFu), (unsigned)pb, (unsigned)dlen);
#endif

    /* 句柄不匹配的包直接丢（PDF 要求先取低 12 位再比对） */
    if ((flags & 0x0FFFu) != (g_conn_handle & 0x0FFFu))
    {
#if BT_TRACE_ACL
        UART1_Printf("[acl] drop: handle mismatch (want %04X)\r\n",
                     (unsigned)(g_conn_handle & 0x0FFFu));
#endif
        return;
    }

    if ((uint16_t)(dlen + 4u) > len)
    {
        return;                       /* 声明长度比实际收到的大，丢弃 */
    }

    if (pb == 0x01u)
    {
        /* 续包：追加 */
        if ((s_aclTotal == 0u) || ((uint32_t)s_aclLen + dlen > BT_ACL_BUF_SIZE))
        {
            s_aclTotal = 0u;
            s_aclLen   = 0u;
            return;
        }
        memcpy(&s_aclBuf[s_aclLen], &data[4], dlen);
        s_aclLen = (uint16_t)(s_aclLen + dlen);
    }
    else
    {
        if (dlen < 4u)
        {
            return;
        }
        l2len = rd16(&data[4]);
        if ((uint32_t)l2len + 4u > BT_ACL_BUF_SIZE)
        {
            return;
        }
        s_aclTotal = (uint16_t)(l2len + 4u);
        memcpy(s_aclBuf, &data[4], dlen);
        s_aclLen = dlen;
    }

    if (s_aclLen < s_aclTotal)
    {
        return;                       /* 还没收齐，等后续分片 */
    }

    /* 收齐了：按 CID 分发 */
    {
        uint16_t cid = rd16(&s_aclBuf[2]);
        uint16_t payloadLen = (uint16_t)(s_aclTotal - 4u);
        uint8_t *payload = &s_aclBuf[4];
        int idx;

        if (cid == L2CAP_CID_SIGNALING)
        {
            /* 信令：可能一个包里串多条信令，逐条走 */
            uint16_t off = 0u;
            while ((uint16_t)(off + 4u) <= payloadLen)
            {
                uint8_t  code = payload[off];
                uint8_t  id   = payload[off + 1u];
                uint16_t slen = rd16(&payload[off + 2u]);

                if ((uint32_t)off + 4u + slen > payloadLen)
                {
                    break;
                }

                /* 每条信令都打一行：这样能看清 Windows 到底来开了哪些通道、
                 * 以及我们回没回。排查"HID Interrupt 没开"这类问题全靠它。 */
#if BT_TRACE_SIG
                UART1_Printf("[l2] sig code=0x%02X id=%u len=%u\r\n",
                             (unsigned)code, (unsigned)id, (unsigned)slen);
#endif

                switch (code)
                {
                    case L2CAP_SIG_CONN_REQ:
                        if (slen >= 4u) { sig_conn_req(&payload[off + 4u], id); }
                        break;

                    case L2CAP_SIG_CONN_RSP:
                        if (slen >= 8u) { sig_conn_rsp(&payload[off + 4u]); }
                        break;

                    case L2CAP_SIG_CONF_REQ:
                        if (slen >= 4u) { sig_conf_req(&payload[off + 4u], slen, id); }
                        break;

                    case L2CAP_SIG_CONF_RSP:
                        if (slen >= 6u) { sig_conf_rsp(&payload[off + 4u], slen); }
                        break;

                    case L2CAP_SIG_DISC_REQ:
                        if (slen >= 4u) { sig_disc_req(&payload[off + 4u], id); }
                        break;

                    case L2CAP_SIG_DISC_RSP:
                        if (slen >= 4u) { sig_disc_rsp(&payload[off + 4u]); }
                        break;

                    case L2CAP_SIG_INFO_REQ:
                        if (slen >= 2u) { sig_info_req(&payload[off + 4u], id); }
                        break;

                    default:
                        break;
                }
                off = (uint16_t)(off + 4u + slen);
            }
        }
        else
        {
            idx = ch_by_lcid(cid);
            if (idx >= 0)
            {
                if (g_chan[idx].psm == L2CAP_PSM_SDP)
                {
                    /* SDP：直接把响应发回去（数据量小，同步即可） */
                    if ((payloadLen >= 5u) &&
                        (payload[0] == SDP_PDU_SERVICE_SEARCH_ATTR_REQ))
                    {
                        uint16_t tid = rd16be(&payload[1]);
                        UART1_Printf("[sdp] search-attr req tid=%u\r\n", (unsigned)tid);
                        sdp_send_attr_rsp(tid, g_chan[idx].rcid);
                    }
                    else if (payloadLen >= 5u)
                    {
                        UART1_Printf("[sdp] pdu=0x%02X (ignored)\r\n",
                                     (unsigned)payload[0]);
                    }
                }
                else if (g_chan[idx].psm == L2CAP_PSM_HID_INTERRUPT)
                {
                    uint16_t n = (payloadLen > 8u) ? 8u : payloadLen;
                    uint16_t j;

                    if ((payloadLen >= 2u) &&
                        ((payload[0] & 0xF0u) == 0xA0u) &&
                        ((payload[0] & 0x0Fu) == HIDP_RPT_TYPE_OUTPUT))
                    {
                        /* Interrupt 通道的 DATA 输出报告是异步消息：处理 LED
                         * 状态即可，不能额外回 HANDSHAKE。Windows 不在此通道
                         * 等待应答；多发一个 0x00 反而可能被当成异常输入包。
                         * Control 通道的 SET_REPORT 仍在 hidp_control_rsp()
                         * 中正常回 HANDSHAKE。 */
                        (void)hidp_handle_output_led(payload, payloadLen);
                    }

                    UART1_Printf("[hid] INT len=%u :", (unsigned)payloadLen);
                    for (j = 0u; j < n; j++)
                    {
                        UART1_Printf(" %02X", (unsigned)payload[j]);
                    }
                    UART1_SendString("\r\n");
                }
                else
                {
                    /* HID Control：主机可能发 HID_CONTROL / GET_REPORT /
                     * SET_REPORT / GET_PROTOCOL 等 */
                    uint16_t n = (payloadLen > 8u) ? 8u : payloadLen;
                    uint16_t j;

                    UART1_Printf("[hid] CTRL len=%u :", (unsigned)payloadLen);
                    for (j = 0u; j < n; j++)
                    {
                        UART1_Printf(" %02X", (unsigned)payload[j]);
                    }
                    UART1_SendString("\r\n");

                    /* 每条请求都必须应答，具体规则见 hidp_control_rsp() */
                    hidp_control_rsp(g_chan[idx].rcid, payload, payloadLen);
                }
            }
            else
            {
                /* 收到一个我们没登记的 CID —— 说明通道表状态和主机不一致，
                 * 这类包如果静默丢掉，就会表现为"主机在说话，我们装没听见" */
                UART1_Printf("[l2] RX unknown cid=0x%04X len=%u\r\n",
                             (unsigned)cid, (unsigned)payloadLen);
            }
        }
    }

    s_aclLen   = 0u;
    s_aclTotal = 0u;
}

static void l2cap_open_channel(int idx)
{
    uint8_t p[4];

    if (g_chan[idx].used != 0u)
    {
        return;                          /* 已经在建或已建好 */
    }

    if (g_chan[idx].lcid == 0u)
    {
        g_chan[idx].lcid = cid_alloc();
    }
    g_chan[idx].used     = 1u;
    g_chan[idx].rcid     = 0u;
    g_chan[idx].cfg_sent = 0u;
    g_chan[idx].cfg_rcvd = 0u;
    g_chan[idx].open     = 0u;

    wr16(&p[0], g_chan[idx].psm);
    wr16(&p[2], g_chan[idx].lcid);
    sig_send(L2CAP_SIG_CONN_REQ, sig_next_id(), p, 4u);

    UART1_Printf("[l2] we initiate psm=0x%04X lcid=0x%04X\r\n",
                 (unsigned)g_chan[idx].psm, (unsigned)g_chan[idx].lcid);
}

void bt_l2cap_open_hid_control(void)
{
    l2cap_open_channel((int)BT_CHAN_HID_CTRL);
}

void bt_l2cap_open_hid_interrupt(void)
{
    if (g_chan[BT_CHAN_HID_CTRL].open == 0u)
    {
        return;                          /* Control 还没配好，先别急 */
    }
    l2cap_open_channel((int)BT_CHAN_HID_INT);
}

/* 诊断用：HID 通道状态位图
 *   bit0 = 控制通道已打开
 *   bit1 = 中断通道已打开
 *   bit2 = 可以发报告（bt_hid_channel_ready） */
uint8_t bt_hid_chan_state(void)
{
    uint8_t st = 0U;

    if (g_chan[BT_CHAN_HID_CTRL].open != 0u) { st |= 0x01U; }
    if (g_chan[BT_CHAN_HID_INT].open  != 0u) { st |= 0x02U; }
    if (bt_hid_channel_ready()        != 0u) { st |= 0x04U; }
    return st;
}

uint8_t bt_hid_channel_ready(void)
{
    return ((g_chan[BT_CHAN_HID_INT].open != 0u) &&
            (g_chan[BT_CHAN_HID_INT].rcid != 0u)) ? 1u : 0u;
}

void bt_hid_send_report(const uint8_t *report, uint16_t len)
{
    if (bt_hid_channel_ready() == 0u)
    {
        return;
    }

    /* HID 报告走 Interrupt 通道，CID 用对端在本通道上分配的 CID。
     * 报告内容 = 0xA1(输入报告头) + ReportID + 报告数据。 */
    l2cap_send(g_chan[BT_CHAN_HID_INT].rcid, report, len);
    g_bt_reports++;
}
