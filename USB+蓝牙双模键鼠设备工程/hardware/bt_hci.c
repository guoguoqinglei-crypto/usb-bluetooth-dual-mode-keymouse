#include <bt_hci.h>

#include <string.h>
#include <stm32f10x.h>

#include <bt_uart.h>
#include <uart.h>
#include <delay.h>

volatile uint8_t  g_bt_state   = BT_STATE_IDLE;
volatile uint32_t g_bt_reports = 0u;
volatile uint8_t  g_bt_error   = 0u;

uint16_t g_conn_handle = BT_HANDLE_NONE;
uint8_t  g_peer_addr[6] = {0};
uint8_t  g_local_addr[6] = {0};

uint16_t g_acl_mtu  = 128u;
uint16_t g_acl_pkts = 4u;
volatile uint16_t g_cmd_credits = 1u;
volatile uint16_t g_acl_credits = 4u;

uint8_t g_link_key[16] = {0};
uint8_t g_link_key_valid = 0u;

volatile uint32_t g_evt_count = 0u;

/* 1 = 本次连接是本机主动发起的（回连） */
uint8_t g_conn_by_us = 0u;

bt_chan_t g_chan[BT_CHAN_COUNT];

static uint8_t  s_rx[BT_RX_BUF_SIZE];
static uint16_t s_rxLen   = 0u;   /* 已收字节数 */
static uint16_t s_rxNeed  = 0u;   /* 当前阶段目标字节数 */
static uint8_t  s_rxHdr   = 0u;   /* 该类型的头长度（不含类型字节） */
static uint8_t  s_rxType  = 0u;
static uint8_t  s_rxPhase = 0u;   /* 0=等类型 1=收头 2=收体 */

/* 发送缓冲：静态区，主栈只有 1KB，不能放局部数组 */
static uint8_t  s_tx[BT_TX_ACL_MAX + 8u];

static uint16_t s_aclRxPending = 0u;

static void bt_return_acl_credits(void)
{
    uint8_t p[5];

    if (s_aclRxPending == 0u)
    {
        return;
    }

    if (g_conn_handle == BT_HANDLE_NONE)
    {
        s_aclRxPending = 0u;
        return;
    }

    if (g_cmd_credits == 0u)
    {
        return;
    }

    /* HCI 规范：1 个句柄 = Num_Handles(1) + Handle(2) + Count(2)，参数共 5 字节。
     * 上一版只发 4 字节（Count 少了高字节），控制器会解析出错。 */
    p[0] = 1u;                                        /* Num_Handles */
    p[1] = (uint8_t)(g_conn_handle & 0xFFu);
    p[2] = (uint8_t)(g_conn_handle >> 8);
    p[3] = (uint8_t)((s_aclRxPending > 255u) ? 255u : s_aclRxPending);
    p[4] = 0u;                                        /* Count 高字节 */

    bt_send_cmd(BT_OP_Host_Num_Completed_Packets, p, 5u);

    s_aclRxPending = 0u;
}

#define BT_HCI_TRACE   1

#if BT_HCI_TRACE
static void bt_trace(const char *tag, const uint8_t *d, uint16_t n)
{
    static const char hexd[] = "0123456789ABCDEF";
    static char line[2400];
    uint16_t i;
    uint16_t p = 0u;

    while ((*tag != '\0') && (p < 4u))
    {
        line[p++] = *tag++;
    }

    for (i = 0u; (i < n) && ((uint16_t)(p + 4u) < (uint16_t)sizeof(line)); i++)
    {
        line[p++] = ' ';
        line[p++] = hexd[d[i] >> 4];
        line[p++] = hexd[d[i] & 0x0Fu];
    }
    line[p] = '\0';

    UART1_SendString(line);
    UART1_SendString("\r\n");
}
#define BT_TRACE_TX(d, n)   bt_trace("TX:", (d), (n))
#define BT_TRACE_RX(d, n)   bt_trace("RX:", (d), (n))
#else
#define BT_TRACE_TX(d, n)   do { } while (0)
#define BT_TRACE_RX(d, n)   do { } while (0)
#endif

/* 把某个类型对应的头长度填出来 */
static uint8_t h4_hdr_len(uint8_t type)
{
    switch (type)
    {
        case H4_TYPE_CMD:   return 3u;   /* Opcode(2) + Plen(1) */
        case H4_TYPE_ACL:   return 4u;   /* Handle(2) + Dlen(2) */
        case H4_TYPE_SCO:   return 3u;   /* Handle(2) + Dlen(1) */
        case H4_TYPE_EVENT: return 2u;   /* EvtCode(1) + Plen(1) */
        default:            return 0u;
    }
}

/* 从已收到的头里算出负载长度 */
static uint16_t h4_payload_len(uint8_t type, const uint8_t *p)
{
    switch (type)
    {
        case H4_TYPE_CMD:   return (uint16_t)p[2];                        /* p[0..1]=opcode */
        case H4_TYPE_ACL:   return (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
        case H4_TYPE_SCO:   return (uint16_t)p[2];
        case H4_TYPE_EVENT: return (uint16_t)p[1];                        /* p[0]=事件码 */
        default:            return 0u;
    }
}

/* 一个完整包到手，按类型分发 */
static void h4_dispatch(void)
{
    uint8_t *body = &s_rx[1];           /* 跳过 H4 类型字节 */

    BT_TRACE_RX(s_rx, s_rxLen);

    switch (s_rxType)
    {
        case H4_TYPE_EVENT:
            bt_evt_dispatch(body, (uint8_t)(s_rxLen - 1u));
            break;

        case H4_TYPE_ACL:
            /* 每收一个 ACL 包都要记账，poll 结束时还给控制器 */
            s_aclRxPending++;
            bt_acl_input(body, (uint16_t)(s_rxLen - 1u));
            break;

        default:
            /* 控制器不会给我们发命令/SCO，忽略即可 */
            break;
    }
}

void bt_transport_reset(void)
{
    s_rxLen   = 0u;
    s_rxNeed  = 0u;
    s_rxHdr   = 0u;
    s_rxPhase = 0u;
    s_rxType  = 0u;
}

void bt_transport_poll(void)
{
    uint8_t b;

    while (BtUart_TryRead(&b) != 0u)
    {
        if (s_rxPhase == 0u)
        {
            /* 等一个合法的 H4 类型字节。噪声直接丢掉，
             * 比"收到就当真"强得多——波特率切错时会有一堆乱码 */
            if (h4_hdr_len(b) == 0u)
            {
                continue;
            }
            s_rxType  = b;
            s_rxHdr   = h4_hdr_len(b);
            s_rx[0]   = b;
            s_rxLen   = 1u;
            s_rxNeed  = (uint16_t)(1u + s_rxHdr);
            s_rxPhase = 1u;
            continue;
        }

        if (s_rxLen >= BT_RX_BUF_SIZE)
        {
            /* 超长包，丢掉重来，避免踩内存 */
            bt_transport_reset();
            continue;
        }

        s_rx[s_rxLen++] = b;

        if (s_rxPhase == 1u && s_rxLen >= s_rxNeed)
        {
            uint16_t payload = h4_payload_len(s_rxType, &s_rx[1]);

            if ((uint32_t)payload + 1u + s_rxHdr > BT_RX_BUF_SIZE)
            {
                bt_transport_reset();       /* 头里声明的长度不合理 */
                continue;
            }
            s_rxNeed  = (uint16_t)(1u + s_rxHdr + payload);
            s_rxPhase = 2u;

            if (s_rxLen >= s_rxNeed)
            {
                h4_dispatch();
                bt_transport_reset();
            }
            continue;
        }

        if (s_rxPhase == 2u && s_rxLen >= s_rxNeed)
        {
            h4_dispatch();
            bt_transport_reset();
        }
    }

    bt_return_acl_credits();
}

void bt_send_cmd(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    s_tx[0] = H4_TYPE_CMD;
    s_tx[1] = (uint8_t)(opcode & 0xFFu);
    s_tx[2] = (uint8_t)(opcode >> 8);
    s_tx[3] = plen;

    if ((plen != 0u) && (params != 0))
    {
        memcpy(&s_tx[4], params, plen);
    }

    if (g_cmd_credits > 0u)
    {
        g_cmd_credits--;
    }

    BT_TRACE_TX(s_tx, (uint16_t)(4u + plen));
    BtUart_Send(s_tx, (uint16_t)(4u + plen));
}

static uint8_t s_frame[4u + BT_TX_ACL_MAX];

void bt_send_acl(uint16_t handle, uint16_t cid,
                 const uint8_t *payload, uint16_t len)
{
    uint16_t total;
    uint16_t off;
    uint16_t fragMax;
    uint16_t fragCnt;
    uint8_t  first = 1u;

    if (len > BT_TX_ACL_MAX)
    {
        return;                              /* 超长直接拒绝，绝不截断后照发 */
    }

    /* 先拼一帧完整的 L2CAP：Length(2) + CID(2) + 负载 */
    total = (uint16_t)(4u + len);
    s_frame[0] = (uint8_t)(len & 0xFFu);
    s_frame[1] = (uint8_t)(len >> 8);
    s_frame[2] = (uint8_t)(cid & 0xFFu);
    s_frame[3] = (uint8_t)(cid >> 8);
    if (len != 0u)
    {
        memcpy(&s_frame[4], payload, len);
    }

    /* 每片最多塞 g_acl_mtu 字节；拿不到有效值就按 128 保守分片 */
    fragMax = (g_acl_mtu >= 32u) ? g_acl_mtu : 128u;
    if (fragMax > (uint16_t)sizeof(s_tx) - 5u)
    {
        fragMax = (uint16_t)(sizeof(s_tx) - 5u);
    }

    off = 0u;
    fragCnt = 0u;
    while (off < total)
    {
        uint16_t n = (uint16_t)(total - off);
        uint16_t h;

        if (n > fragMax)
        {
            n = fragMax;
        }

        s_tx[0] = H4_TYPE_ACL;

        h = (uint16_t)(handle & 0x0FFFu);
        h = (uint16_t)(h | (first != 0u ? 0x2000u : 0x1000u));
        s_tx[1] = (uint8_t)(h & 0xFFu);
        s_tx[2] = (uint8_t)(h >> 8);

        s_tx[3] = (uint8_t)(n & 0xFFu);
        s_tx[4] = (uint8_t)(n >> 8);

        memcpy(&s_tx[5], &s_frame[off], n);

        BT_TRACE_TX(s_tx, (uint16_t)(5u + n));
        BtUart_Send(s_tx, (uint16_t)(5u + n));

        off = (uint16_t)(off + n);
        fragCnt++;
        first = 0u;
    }

    /* 每个 ACL 分片各占一个控制器发送缓冲：按分片数扣，不是按 PDU 扣。
     * 上一版整个 L2CAP PDU 只扣 1，信用计数会越算越松，最终超发。 */
    if (g_acl_credits >= fragCnt)
    {
        g_acl_credits = (uint16_t)(g_acl_credits - fragCnt);
    }
    else
    {
        g_acl_credits = 0u;
    }
}

void bt_flush_acl_credits(void)
{
    uint8_t p[5];

    if (g_conn_handle == BT_HANDLE_NONE)
    {
        return;
    }

    /* 5 字节参数，Count 是 16 位：高字节必须在线上出现 */
    p[0] = 1u;                                        /* Num_Handles */
    p[1] = (uint8_t)(g_conn_handle & 0xFFu);
    p[2] = (uint8_t)(g_conn_handle >> 8);
    p[3] = 1u;                                        /* Complete_Count 低字节 */
    p[4] = 0u;                                        /* Complete_Count 高字节 */

    bt_send_cmd(BT_OP_Host_Num_Completed_Packets, p, 5u);
}
