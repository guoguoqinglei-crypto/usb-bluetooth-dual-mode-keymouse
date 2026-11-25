/* ============================================================================
 * hci_acl.c —— ACL 重组 + L2CAP 信令 + SDP 服务记录（移植自
 *             ESP32_VHCI_demo/main/hci_acl.c）
 * ----------------------------------------------------------------------------
 * 与 ESP32 版的差别：
 *   1. SDP 响应 662 字节，ESP32 那边控制器 MTU 有 1021 所以一包发完；
 *      RDA5876 的 MTU 通常只有 128，这里交给 LMacl_send() 自动分片。
 *   2. 补了"本机主动建 HID 通道"的分支（回连时走这条）。
 *   3. SDP 属性表保持与 ESP32 逐字节一致 —— 那张表是讲师从空中包抄出来的，
 *      改一个长度字节就可能让电脑认不出服务。
 * ========================================================================== */

#include "app_bt.h"

#include <string.h>
#include <stm32f10x.h>

#include <delay.h>
#include <uart.h>

#define BT_TRACE  1

/* ---------------- L2CAP 常量 ---------------- */
/* L2CAP / PSM / 信令常量都在 hci_acl.h 里（用 demo 的命名） */

/* ---------------- 通道表 ---------------- */
typedef struct
{
    uint16_t lcid;        /* 本地分配的源 CID */
    uint16_t rcid;        /* 对端的目的 CID（发数据时填这个） */
    uint16_t psm;
    uint8_t  opened;      /* 连接已响应 */
    uint8_t  configured;  /* 配置已完成 */
} BtChan;

/* 通道槽位：名字对齐 demo 的 INDEX_xxx */
#define CH_SDP       INDEX_SDP
#define CH_HID_CTRL  INDEX_HID_CONTROL
#define CH_HID_INT   INDEX_HID_INTERRUPT
#define CH_NUM       3

static BtChan   s_ch[CH_NUM];
static uint16_t s_nextCid = 0x0040U;

/* 回连时建通道的顺序 */
enum
{
    OPEN_IDLE = 0,
    OPEN_WAIT_CTRL_RSP,
    OPEN_WAIT_CTRL_CFG,
    OPEN_WAIT_INT_RSP,
    OPEN_WAIT_INT_CFG,
    OPEN_DONE
};

static uint8_t s_openState = OPEN_IDLE;

/* ACL 重组缓冲（L2CAP 帧最大 1024，足够 SDP 的 653） */
#define PDU_MAX  1024U
static uint8_t  s_pdu[PDU_MAX];
static uint16_t s_pduLen;
static uint16_t s_pduNeed;

/* 发送缓冲：L2CAP 头(4) + SDP 响应(653) */
static uint8_t  s_out[4U + 700U];

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

void l2cap_reset(void)
{
    memset(s_ch, 0, sizeof(s_ch));
    s_nextCid   = 0x0040U;
    s_pduLen    = 0U;
    s_pduNeed   = 0U;
    s_openState = OPEN_IDLE;
}

static uint16_t cid_alloc(void)
{
    uint16_t c = s_nextCid;
    s_nextCid++;
    if (s_nextCid == 0U)
    {
        s_nextCid = 0x0040U;
    }
    return c;
}

static int8_t ch_by_psm(uint16_t psm)
{
    if (psm == PSM_SDP)      return (int8_t)CH_SDP;
    if (psm == PSM_HID_CTRL) return (int8_t)CH_HID_CTRL;
    if (psm == PSM_HID_INT)  return (int8_t)CH_HID_INT;
    return -1;
}

static int8_t ch_by_lcid(uint16_t cid)
{
    uint8_t i;
    for (i = 0U; i < CH_NUM; i++)
    {
        if ((s_ch[i].lcid != 0U) && (s_ch[i].lcid == cid))
        {
            return (int8_t)i;
        }
    }
    return -1;
}

/* 对端 CID 反查通道：Configuration Response 里的 Source CID 是**对端**的 CID */
static int8_t ch_by_rcid(uint16_t cid)
{
    uint8_t i;
    for (i = 0U; i < CH_NUM; i++)
    {
        if ((s_ch[i].rcid != 0U) && (s_ch[i].rcid == cid))
        {
            return (int8_t)i;
        }
    }
    return -1;
}

/* ==========================================================================
 * 发送一层：L2CAP 帧 -> ACL -> 控制器
 * ========================================================================== */
static void l2cap_send(uint16_t cid, const uint8_t *payload, uint16_t len)
{
    if ((uint16_t)(len + 4U) > sizeof(s_out))
    {
        return;
    }

    put16(&s_out[0], len);
    put16(&s_out[2], cid);
    memcpy(&s_out[4], payload, len);

    LMacl_send(conn_handle, 0x02U, s_out, (uint16_t)(len + 4U));
}

static void sig_send(uint8_t code, uint8_t id, const uint8_t *payload, uint16_t plen)
{
    uint8_t  buf[12U + 32U];
    uint16_t pos = 0U;

    if (plen > 32U)
    {
        return;
    }

    buf[pos++] = code;
    buf[pos++] = id;
    put16(&buf[pos], plen);
    pos = (uint16_t)(pos + 2U);

    if ((plen > 0U) && (payload != 0))
    {
        memcpy(&buf[pos], payload, plen);
        pos = (uint16_t)(pos + plen);
    }

    l2cap_send(CID_SIGNALING, buf, pos);
}

/* ==========================================================================
 * SDP：Search Attribute Response
 * ========================================================================== */
static const unsigned char sdp_attr_list[] =
{
	//======================================================================
	//ServiceRecord1:PnPInformation(服务句柄:0x00010002)
	//======================================================================
	0x36,0x00,0x7B,//AttributeList(总长度:123字节)

	0x09,0x00,0x00,//Attribute:ServiceRecordHandle 00 00
		0x0A,0x00,0x01,0x00,0x02, //0x00 01 00 02

	0x09,0x00,0x01,//Attribute:ServiceClassIDList-UUID: 00 01
		0x36,0x00,0x03,0x19,0x12,0x00, //PnPInformation(0x1200)

	0x09,0x00,0x0D,//Attribute:AdditionalProtocolDescriptorLists 00 0D
		0x36,0x00,0x12,0x36,0x00,0x0F,0x36,0x00,0x06,0x19,0x01,0x00,// UUID:L2CAP
		0x09,0x00,0x01,//PSM:0x0001
		0x36,0x00,0x03,0x19,0x00,0x01,//UUID: SDP

	0x09,0x00,0x02,//Attribute:ServiceRecordState 00 02
		0x0A,0x00,0x00,0x02,0xDB,//Unsigned Integer: 731

	0x09,0x00,0x05,//Attribute:BrowseGroupList 00 05
		0x36,0x00,0x03,0x19,0x10,0x02,//UUID:PublicBrowseGroup(0x1002)

	0x09,0x00,0x06,//Attribute:LanguageBaseAttributeIDList 00 06
		0x36,0x00,0x09,0x09,0x65,0x6E,//语言标识符:ENGLISH('en')
		0x09,0x00,0x6A,//字符编码:0x006A
		0x09,0x01,0x00,//语言基准ID:0x0100

	0x09,0x00,0x09,//Attribute:BluetoothProfileDescriptorList 00 09
		0x36,0x00,0x09,0x36,0x00,0x06,0x19,0x12,0x00,//UUID:PnPInformation(0x1200)
		0x09,0x01,0x00,//v1.0(0x0100)

	0x09,0x02,0x00,//Attribute:SpecificationID-DeviceIDSpecVersion
		0x09,0x01,0x03,//v1.3

	0x09,0x02,0x01,//Attribute:VendorID 02 01
		0x09,0x04,0x5E,//0x045E

	0x09,0x02,0x02,//Attribute:ProductID 02 02
		0x09,0x02,0xE0,//0x02E0

	0x09,0x02,0x03,//Attribute:Version-ProductVersion 02 03
		0x09,0x09,0x03,//Product Version: 9.0.3

	0x09,0x02,0x04,//Attribute:PrimaryRecord 02 04
		0x28,0x01,//True

	0x09,0x02,0x05,//Attribute:VendorIDSource 02 05
		0x09,0x00,0x02,//USBImplementer'sForum

	//======================================================================
	//ServiceRecord2:HID游戏手柄(服务句柄:0x00010001)
	//======================================================================
	0x36,0x02,0x01,//AttributeList(总长度:513字节)

	0x09,0x00,0x00,////Attribute:ServiceRecordHandle 00 00
		0x0A,0x00,0x01,0x00,0x01,// 00 01 00 01

	0x09,0x00,0x01,//Attribute:ServiceClassIDList 00 01
		0x36,0x00,0x03,0x19,0x11,0x24,//UUID:HIDService(0x1124)

	0x09,0x00,0x04,//Attribute:ProtocolDescriptorList 00 04
		0x36,0x00,0x0F,0x36,0x00,0x06,0x19,0x01,0x00,// UUID:L2CAP
		0x09,0x00,0x11,//PSM:0x0011(HIDControl)
		0x36,0x00,0x03,0x19,0x00,0x11,//HIDProtocol

	0x09,0x00,0x06,//Attribute:LanguageBaseAttributeIDList 00 06
		0x36,0x00,0x09,0x09,0x65,0x6E,//语言标识符:ENGLISH('en')
		0x09,0x00,0x6A,//字符编码:0x006A
		0x09,0x01,0x00,//语言基准ID:0x0100

	0x09,0x00,0x0D,//Attribute:AdditionalProtocolDescriptorLists 00 0D
		0x36,0x00,0x12,0x36,0x00,0x0F,0x36,0x00,0x06,0x19,0x01,0x00,// UUID:L2CAP
		0x09,0x00,0x13,//PSM:0x0013(HIDInterrupt)
		0x36,0x00,0x03,0x19,0x00,0x11,//HIDProtocol

	0x09,0x01,0x00,//Attribute:ServiceName 01 00
		0x25,0x16,//字符串长度(0X16 22字节)
		0x58,0x62,0x6F,0x78,0x20,0x42,0x6C,0x75,0x65,0x74,0x6F,0x6F,0x74,0x68,0x20,0x47,0x61,0x6D,0x65,0x70,0x61,0x64,//"XboxBluetoothGamepad"

	0x09,0x00,0x09,//Attribute:BluetoothProfileDescriptorList 00 09
		0x36,0x00,0x09,0x36,0x00,0x06,0x19,0x11,0x24,//UUID:HIDService(0x1124)
		0x09,0x01,0x01,//版本:v1.1(0x0101)

	0x09,0x02,0x01,//Attribute:HIDParserVersion 02 01
		0x09,0x01,0x11,//版本:v1.1(0x0111)

	0x09,0x02,0x02,//Attribute:HIDDeviceSubClass 02 02
		0x08,0x40,//0x40

	0x09,0x02,0x03,//Attribute:HIDCountryCode 00 03
		0x08,0x21,//0x21

	0x09,0x02,0x04,//Attribute:HIDVirtualCable 02 04
		0x28,0x01,//True

	0x09,0x02,0x05,//Attribute:HIDReconnectInitiate 02 05
		0x28,0x01,//True

	0x09,0x02,0x06,//Attribute:HIDReportDescriptor 02 06
		0x36,0x01,0x3A,0x36,0x01,0x37,0x08,0x22,//0x22  Class Descriptor Type: Report
		0x26,0x01,0x32,//Report Descriptor Length: 0x0132 306字节
		0x05,0x01,0x09,0x05,0xA1,0x01,0x85,0x01,0x09,0x01,0xA1,0x00,
		0x09,0x30,0x09,0x31,0x15,0x00,0x27,0xFF,0xFF,0x00,0x00,0x95,
		0x02,0x75,0x10,0x81,0x02,0xC0,0x09,0x01,0xA1,0x00,0x09,0x33,
		0x09,0x34,0x15,0x00,0x27,0xFF,0xFF,0x00,0x00,0x95,0x02,0x75,
		0x10,0x81,0x02,0xC0,0x05,0x01,0x09,0x32,0x15,0x00,0x26,0xFF,
		0x03,0x95,0x01,0x75,0x0A,0x81,0x02,0x15,0x00,0x25,0x00,0x75,
		0x06,0x95,0x01,0x81,0x03,0x05,0x01,0x09,0x35,0x15,0x00,0x26,
		0xFF,0x03,0x95,0x01,0x75,0x0A,0x81,0x02,0x15,0x00,0x25,0x00,
		0x75,0x06,0x95,0x01,0x81,0x03,0x05,0x01,0x09,0x39,0x15,0x01,
		0x25,0x08,0x35,0x00,0x46,0x3B,0x01,0x66,0x14,0x00,0x75,0x04,
		0x95,0x01,0x81,0x42,0x75,0x04,0x95,0x01,0x15,0x00,0x25,0x00,
		0x35,0x00,0x45,0x00,0x65,0x00,0x81,0x03,0x05,0x09,0x19,0x01,
		0x29,0x0A,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x0A,0x81,0x02,
		0x15,0x00,0x25,0x00,0x75,0x01,0x95,0x06,0x81,0x03,0x05,0x01,
		0x09,0x80,0x85,0x02,0xA1,0x00,0x09,0x85,0x15,0x00,0x25,0x01,
		0x95,0x01,0x75,0x01,0x81,0x02,0x15,0x00,0x25,0x00,0x75,0x07,
		0x95,0x01,0x81,0x03,0xC0,0x05,0x0F,0x09,0x21,0x85,0x03,0xA1,
		0x02,0x09,0x97,0x15,0x00,0x25,0x01,0x75,0x04,0x95,0x01,0x91,
		0x02,0x15,0x00,0x25,0x00,0x75,0x04,0x95,0x01,0x91,0x03,0x09,
		0x70,0x15,0x00,0x25,0x64,0x75,0x08,0x95,0x04,0x91,0x02,0x09,
		0x50,0x66,0x01,0x10,0x55,0x0E,0x15,0x00,0x26,0xFF,0x00,0x75,
		0x08,0x95,0x01,0x91,0x02,0x09,0xA7,0x15,0x00,0x26,0xFF,0x00,
		0x75,0x08,0x95,0x01,0x91,0x02,0x65,0x00,0x55,0x00,0x09,0x7C,
		0x15,0x00,0x26,0xFF,0x00,0x75,0x08,0x95,0x01,0x91,0x02,0xC0,
		0x05,0x06,0x09,0x20,0x85,0x04,0x15,0x00,0x26,0xFF,0x00,0x75,
		0x08,0x95,0x01,0x81,0x02,0xC0,

	0x09,0x02,0x07,//Attribute: HIDLANGIDBaseList 02 07
		0x36,0x00,0x09,0x36,0x00,0x06,0x09,0x04,0x09,//LANGID: 0x0409
		0x09,0x01,0x00,//LanguageBase: 0x0100

	0x09,0x02,0x0A,//Attribute: HIDRemoteWake 02 0A
		0x28,0x00,//False

	0x09,0x02,0x0C,//Attribute: HIDVersion 02 0C
		0x09,0x0C,0x80,//Supervison Timeout 0X0C80

	0x09,0x02,0x0D,//Attribute: HIDVirtualCable 02 0D
		0x28,0x01,//True

	0x09,0x02,0x0E,//Attribute: HIDBootDevice 02 0E
		0x28,0x00,//False

	0x09,0x02,0x0F,//Attribute: HIDSSRHostMaxLatency 02 0F
		0x09,0x06,0x40,//SSR Host Max Latency: 0x0640

	0x09,0x02,0x10,//Attribute: HIDSSRHostMinTimeout 02 10
		0x09,0x0C,0x80,//SSR Host Min Timeout: 0x0C80
};

#define SDP_ATTR_LIST_LEN   ((uint16_t)sizeof(sdp_attr_list))

static void sdp_send_attr_rsp(uint8_t tid0, uint8_t tid1, uint16_t dstCid)
{
    static uint8_t payload[8U + 3U + sizeof(sdp_attr_list) + 1U];
    uint16_t pos = 0U;
    uint16_t attrLen;
    uint16_t paramLen;

    /* AttributeList = 外层 SEQUENCE 头(3) + 两条服务记录 */
    attrLen  = (uint16_t)(3U + SDP_ATTR_LIST_LEN);
    paramLen = (uint16_t)(2U + 1U + attrLen);   /* ByteCount(2) + AttrList + ContinuationState(1) */

    payload[pos++] = SDP_SEARCH_ATTR_RSP;       /* PDU ID 0x07 */
    payload[pos++] = tid0;                      /* Transaction ID 原样回填 */
    payload[pos++] = tid1;
    payload[pos++] = (uint8_t)(paramLen >> 8);  /* SDP 内部一律大端 */
    payload[pos++] = (uint8_t)(paramLen & 0xFFU);
    payload[pos++] = (uint8_t)(attrLen >> 8);
    payload[pos++] = (uint8_t)(attrLen & 0xFFU);

    payload[pos++] = 0x36U;                     /* SEQUENCE */
    payload[pos++] = (uint8_t)((SDP_ATTR_LIST_LEN >> 8) & 0xFFU);
    payload[pos++] = (uint8_t)(SDP_ATTR_LIST_LEN & 0xFFU);
    memcpy(&payload[pos], sdp_attr_list, SDP_ATTR_LIST_LEN);
    pos = (uint16_t)(pos + SDP_ATTR_LIST_LEN);
    payload[pos++] = 0x00U;                     /* ContinuationState = 0 */

    l2cap_send(dstCid, payload, pos);
}

static void sdp_recv(const uint8_t *p, uint16_t len)
{
    uint8_t pduId;

    if (len < 5U)
    {
        return;
    }

    pduId = p[0];

    if (pduId == SDP_SEARCH_ATTR_REQ)
    {
        uint16_t dst = (s_ch[CH_SDP].rcid != 0U) ? s_ch[CH_SDP].rcid : CID_SIGNALING;
        UART1_SendString("[bt] SDP search attr req\r\n");
        sdp_send_attr_rsp(p[1], p[2], dst);
    }
    else
    {
        UART1_Printf("[bt] sdp pdu=0x%02X (ignored)\r\n", (unsigned)pduId);
    }
}

/* ==========================================================================
 * L2CAP 信令
 * ========================================================================== */
static void conf_req_send(uint16_t peerCid)
{
    uint8_t pl[4];
    put16(&pl[0], peerCid);
    put16(&pl[2], 0x0000U);        /* Flags：无选项 */
    sig_send(SIG_CONF_REQ, 0x01U, pl, 4U);
}

void l2cap_open_hid(void)
{
    uint16_t scid;

    if ((s_ch[CH_HID_CTRL].opened != 0U) || (s_openState != OPEN_IDLE))
    {
        return;
    }

    scid = cid_alloc();
    s_ch[CH_HID_CTRL].psm  = PSM_HID_CTRL;
    s_ch[CH_HID_CTRL].lcid = scid;
    s_ch[CH_HID_CTRL].opened = 0U;

    {
        uint8_t pl[4];
        put16(&pl[0], PSM_HID_CTRL);
        put16(&pl[2], scid);
        sig_send(SIG_CONN_REQ, 0x10U, pl, 4U);
    }

    s_openState = OPEN_WAIT_CTRL_RSP;
    UART1_SendString("[bt] open HID control channel\r\n");
}

static void sig_recv(const uint8_t *p, uint16_t len)
{
    uint8_t  code;
    uint8_t  id;
    uint16_t plen;
    const uint8_t *pl;

    if (len < 4U)
    {
        return;
    }

    code = p[0];
    id   = p[1];
    plen = le16(&p[2]);
    pl   = &p[4];

    if ((uint16_t)(4U + plen) > len)
    {
        return;
    }

    switch (code)
    {
        case SIG_CONN_REQ:
        {
            uint16_t psm;
            uint16_t scid;
            int8_t   idx;
            uint8_t  rsp[8];

            if (plen < 4U)
            {
                break;
            }

            psm  = le16(&pl[0]);
            scid = le16(&pl[2]);
            idx  = ch_by_psm(psm);

            if ((idx < 0) || (s_ch[idx].lcid != 0U))
            {
                put16(&rsp[0], scid);
                put16(&rsp[2], 0U);
                put16(&rsp[4], (idx < 0) ? RSP_PSM_NOT_SUPPORTED : RSP_NO_RESOURCES);
                put16(&rsp[6], 0U);
                sig_send(SIG_CONN_RSP, id, rsp, 8U);
                UART1_Printf("[bt] conn req psm=0x%04X refused\r\n", (unsigned)psm);
                break;
            }

            s_ch[idx].psm    = psm;
            s_ch[idx].rcid   = scid;
            s_ch[idx].lcid   = cid_alloc();
            s_ch[idx].opened = 1U;

            put16(&rsp[0], scid);                    /* Destination CID = 对端 CID */
            put16(&rsp[2], s_ch[idx].lcid);          /* Source CID = 本地 CID */
            put16(&rsp[4], 0x0000U);                 /* Result = success */
            put16(&rsp[6], 0x0000U);                 /* Status = 0 */
            sig_send(SIG_CONN_RSP, id, rsp, 8U);

            /* 紧接着发起本端配置（两个方向都要配对完成才算通道打开） */
            conf_req_send(scid);

            UART1_Printf("[bt] conn req psm=0x%04X ok lcid=0x%04X\r\n",
                         (unsigned)psm, (unsigned)s_ch[idx].lcid);
            break;
        }

        case SIG_CONN_RSP:
        {
            uint16_t myCid;
            uint16_t peerCid;
            uint16_t result;
            int8_t   idx;

            if (plen < 8U)
            {
                break;
            }

            /* L2CAP 规范：响应里 Destination CID 是**请求方**（本机）的 CID，
             * Source CID 是**响应方**（对端）的 CID —— 两个方向都要按这个来填。 */
            myCid   = le16(&pl[0]);
            peerCid = le16(&pl[2]);
            result  = le16(&pl[4]);
            idx     = ch_by_lcid(myCid);

            if (idx < 0)
            {
                break;
            }

            if (result != 0U)
            {
                UART1_Printf("[bt] conn rsp fail result=0x%04X\r\n", (unsigned)result);
                s_openState = OPEN_IDLE;
                break;
            }

            s_ch[idx].rcid   = peerCid;
            s_ch[idx].opened = 1U;

            if ((uint8_t)idx == (uint8_t)CH_HID_CTRL)
            {
                s_openState = OPEN_WAIT_CTRL_CFG;
            }
            else if ((uint8_t)idx == (uint8_t)CH_HID_INT)
            {
                s_openState = OPEN_WAIT_INT_CFG;
            }

            /* 配置请求发给对端的 CID */
            conf_req_send(peerCid);
            break;
        }

        case SIG_CONF_REQ:
        {
            uint16_t dcid;
            uint8_t  rsp[6];

            if (plen < 4U)
            {
                break;
            }

            dcid = le16(&pl[0]);

            put16(&rsp[0], dcid);
            put16(&rsp[2], 0x0000U);                 /* Flags */
            put16(&rsp[4], 0x0000U);                 /* Result = success */
            sig_send(SIG_CONF_RSP, id, rsp, 6U);
            break;
        }

        case SIG_CONF_RSP:
        {
            uint16_t peerCid;
            uint16_t result;
            int8_t   idx;

            if (plen < 6U)
            {
                break;
            }

            /* 响应里的 Source CID 是对端那一端的 CID，用 rcid 反查 */
            peerCid = le16(&pl[0]);
            result  = le16(&pl[4]);
            idx     = ch_by_rcid(peerCid);

            if (idx < 0)
            {
                break;
            }

            if (result != 0U)
            {
                UART1_Printf("[bt] conf rsp fail result=0x%04X\r\n", (unsigned)result);
                break;
            }

            s_ch[idx].configured = 1U;

            /* 回连时按 ctrl -> interrupt 的顺序推进 */
            if (((uint8_t)idx == (uint8_t)CH_HID_CTRL) &&
                (s_openState == OPEN_WAIT_CTRL_CFG))
            {
                uint16_t ncid = cid_alloc();
                uint8_t  req[4];

                s_ch[CH_HID_INT].psm  = PSM_HID_INT;
                s_ch[CH_HID_INT].lcid = ncid;

                put16(&req[0], PSM_HID_INT);
                put16(&req[2], ncid);
                sig_send(SIG_CONN_REQ, 0x11U, req, 4U);

                s_openState = OPEN_WAIT_INT_RSP;
            }
            else if (((uint8_t)idx == (uint8_t)CH_HID_INT) &&
                     (s_openState == OPEN_WAIT_INT_CFG))
            {
                s_openState = OPEN_DONE;
                bt_state = 4U;
                UART1_SendString("[bt] HID channels ready\r\n");
                hci_write_scan_enable(0x00U);   /* 连上后关扫描，别占天线 */
            }
            break;
        }

        case SIG_DISC_REQ:
        {
            uint8_t rsp[4];

            if (plen < 4U)
            {
                break;
            }

            put16(&rsp[0], le16(&pl[2]));
            put16(&rsp[2], le16(&pl[0]));
            sig_send(SIG_DISC_RSP, id, rsp, 4U);

            {
                int8_t idx = ch_by_lcid(le16(&pl[0]));
                if (idx >= 0)
                {
                    UART1_SendString("[bt] channel closed\r\n");
                    memset(&s_ch[idx], 0, sizeof(BtChan));
                }
            }
            break;
        }

        case SIG_INFO_REQ:
        {
            uint8_t rsp[4];

            if (plen < 2U)
            {
                break;
            }

            put16(&rsp[0], le16(&pl[0]));
            put16(&rsp[2], 0x0001U);        /* Not supported */
            sig_send(SIG_INFO_RSP, id, rsp, 4U);
            break;
        }

        default:
            UART1_Printf("[bt] sig code=0x%02X (ignored)\r\n", (unsigned)code);
            break;
    }
}

/* ==========================================================================
 * ACL 接收：重组 -> 按 CID 分发
 * ========================================================================== */
void LMacl_receive(uint8_t pb, const uint8_t *data, uint16_t len)
{
    uint16_t cid;

    if ((data == 0) || (len == 0U))
    {
        return;
    }

    if (pb == 0x01U)
    {
        /* 续片：直接往缓冲尾部追加（续片可以短于 4 字节，不能再按包头判） */
        if ((s_pduNeed == 0U) || ((uint32_t)s_pduLen + len > PDU_MAX))
        {
            s_pduLen  = 0U;
            s_pduNeed = 0U;
            return;
        }
        memcpy(&s_pdu[s_pduLen], data, len);
        s_pduLen = (uint16_t)(s_pduLen + len);
    }
    else
    {
        /* 首片：至少要装得下 L2CAP 头（Length + CID） */
        uint16_t total;
        uint16_t copy = len;

        if (len < 4U)
        {
            return;
        }

        total = (uint16_t)(le16(data) + 4U);

        if ((total > PDU_MAX) || (copy > total))
        {
            return;
        }

        memcpy(s_pdu, data, copy);
        s_pduLen  = copy;
        s_pduNeed = total;
    }

    if (s_pduLen < s_pduNeed)
    {
        return;                     /* 还没凑齐，等下一片 */
    }

    cid = le16(&s_pdu[2]);

    if (cid == CID_SIGNALING)
    {
        sig_recv(&s_pdu[4], (uint16_t)(s_pduLen - 4U));
    }
    else
    {
        int8_t idx = ch_by_lcid(cid);

        if (idx == (int8_t)CH_SDP)
        {
            sdp_recv(&s_pdu[4], (uint16_t)(s_pduLen - 4U));
        }
        else if (idx == (int8_t)CH_HID_CTRL)
        {
            if (s_pdu[4] == 0xA2U)
            {
                UART1_Printf("[bt] HID output report len=%u\r\n",
                             (unsigned)(s_pduLen - 5U));
            }
        }
        else if (idx == (int8_t)CH_HID_INT)
        {
            /* 中断通道主机侧基本不发数据，收到就忽略 */
        }
    }

    s_pduLen  = 0U;
    s_pduNeed = 0U;
}

/* ==========================================================================
 * HID 输入报告
 * ========================================================================== */
uint8_t l2cap_hid_ready(void)
{
    return ((s_ch[CH_HID_INT].opened != 0U) &&
            (s_ch[CH_HID_INT].configured != 0U) &&
            (s_ch[CH_HID_INT].rcid != 0U)) ? 1U : 0U;
}

void l2cap_send_hid_input(const uint8_t *data, uint16_t len)
{
    if (!l2cap_hid_ready() || (len > 600U))
    {
        return;
    }

    put16(&s_out[0], len);
    put16(&s_out[2], s_ch[CH_HID_INT].rcid);
    memcpy(&s_out[4], data, len);

    LMacl_send(conn_handle, 0x02U, s_out, (uint16_t)(len + 4U));
    bt_reports++;
}
