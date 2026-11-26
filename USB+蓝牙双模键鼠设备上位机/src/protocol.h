#ifndef PROTOCOL_H
#define PROTOCOL_H

/* USB 和蓝牙共用 9 字节自定义 HID 输入报告体，输出报告仍为 8 字节。
 * USB 是无 Report ID 的独立接口，Windows 前缀填 0；蓝牙是 ID3 的顶层集合，
 * Windows 缓冲区前缀填 3。rep[8] bit0/bit1 分别表示 L3/Shift、R3/Ctrl。 */

#include <QtGlobal>
#include <QString>
#include <QByteArray>
#include <QStringList>

namespace Proto
{
constexpr quint16 kVid       = 0xCAFE;
constexpr quint16 kPid       = 0x4004;
constexpr quint16 kBtVid     = 0x1209;
constexpr quint16 kBtPid     = 0x0001;
constexpr quint16 kUsagePage = 0xFF00;
constexpr quint16 kUsage     = 0x0001;
constexpr quint8 kUsbReportId = 0;
constexpr quint8 kBtReportId  = 3;

constexpr int kInputReportSize       = 9;
constexpr int kLegacyInputReportSize = 8;
constexpr int kInputXferSize         = kInputReportSize + 1;
constexpr int kLegacyInputXferSize   = kLegacyInputReportSize + 1;
constexpr int kOutputReportSize      = 8;
constexpr int kXferSize              = kOutputReportSize + 1;  /* 输出帧仍为 9B */

constexpr quint8 kCmdSetRgb       = 0x01;
constexpr quint8 kCmdGetStatus    = 0x02;
constexpr quint8 kCmdRainbowStart = 0x03;
constexpr quint8 kCmdRainbowStop  = 0x04;

struct KeyDef
{
    const char *name;
    const char *function;
};

inline const KeyDef kKeys[10] = {
    { "A",  "鼠标左键"     },
    { "B",  "鼠标右键"     },
    { "X",  "Caps Lock"    },
    { "Y",  "Num Lock"     },
    { "UP", "空格 Space"   },
    { "DN", "回车 Enter"   },
    { "LF", "鼠标中键"     },
    { "RG", "长按 2 秒切 USB/蓝牙" },
    { "L3", "左 Shift" },
    { "R3", "左 Ctrl"  },
};

inline QByteArray makeFrame(quint8 reportId, quint8 cmd,
                            quint8 b1 = 0, quint8 b2 = 0,
                            quint8 b3 = 0, quint8 b4 = 0,
                            quint8 b5 = 0, quint8 b6 = 0,
                            quint8 b7 = 0)
{
    QByteArray f(kXferSize, char(0));
    f[0] = char(reportId);
    f[1] = char(cmd);
    f[2] = char(b1);
    f[3] = char(b2);
    f[4] = char(b3);
    f[5] = char(b4);
    f[6] = char(b5);
    f[7] = char(b6);
    f[8] = char(b7);
    return f;
}

inline QByteArray makeRgb(int r, int g, int b, quint8 reportId)
{
    auto clamp = [](int v) -> quint8 {
        return quint8(v < 0 ? 0 : (v > 100 ? 100 : v));
    };
    return makeFrame(reportId, kCmdSetRgb, clamp(r), clamp(g), clamp(b));
}

/* 同时兼容新 9B 输入报告和旧 8B 输入报告；输出协议始终为 8B。 */
inline QByteArray stripReportId(const QByteArray &raw, quint8 reportId)
{
    if (raw.size() == kInputXferSize && quint8(raw.at(0)) == reportId)
        return raw.mid(1, kInputReportSize);
    if (raw.size() == kLegacyInputXferSize && quint8(raw.at(0)) == reportId)
        return raw.mid(1, kLegacyInputReportSize);
    if (raw.size() == kInputReportSize || raw.size() == kLegacyInputReportSize)
        return raw;
    return QByteArray();
}

inline QString hexDump(const QByteArray &ba)
{
    QStringList parts;
    parts.reserve(ba.size());
    for (int i = 0; i < ba.size(); ++i)
        parts << QString("%1").arg(quint8(ba.at(i)), 2, 16, QChar('0')).toUpper();
    return parts.join(QLatin1Char(' '));
}

/* 自定义报告 rep[3] = 状态字节（设备侧 5.43 版起上报）：
 *   bit0 = 1  当前是「强制蓝牙」模式（长按 RG 切换）
 *   bit1 = 1  蓝牙 HID 通道已就绪
 * 旧固件这一字节恒为 0，按"自动 / 蓝牙未连接"显示即可。 */
constexpr quint8 kStForceBt  = 0x01;   /* bit0：当前键鼠输出走蓝牙 */
constexpr quint8 kStBtLinked = 0x02;   /* bit1：蓝牙 HID 链路已就绪 */
constexpr quint8 kStBtPref   = 0x04;   /* bit2：用户把模式设成了"强制蓝牙" */

inline QString statusText(quint8 st)
{
    const bool outBt  = (st & kStForceBt)  != 0;
    const bool linked = (st & kStBtLinked) != 0;
    const bool pref   = (st & kStBtPref)   != 0;

    QString mode;
    if (pref) {
        /* 用户选了强制蓝牙：可能已连上（走蓝牙），也可能暂时回落到 USB */
        mode = outBt ? QString::fromUtf8("强制蓝牙")
                     : QString::fromUtf8("强制蓝牙（蓝牙未连，暂走 USB）");
    } else {
        mode = outBt ? QString::fromUtf8("蓝牙（未检测到 USB）")
                     : QString::fromUtf8("自动（USB 优先）");
    }

    const QString link = linked ? QString::fromUtf8("蓝牙已连接")
                                : QString::fromUtf8("蓝牙未连接");
    return mode + QString::fromUtf8("，") + link;
}

inline QList<int> bitmapToKeys(quint8 bm)
{
    QList<int> out;
    for (int i = 0; i < 8; ++i)
        if ((bm & (1u << i)) != 0u) out.append(i);
    return out;
}

inline QString bitmapToText(quint8 bm, quint8 stick = 0)
{
    QStringList names;
    const QList<int> keys = bitmapToKeys(bm);
    for (int i : keys)
        names.append(QString::fromUtf8(kKeys[i].name));
    for (int i = 0; i < 2; ++i)
        if ((stick & (1u << i)) != 0u)
            names.append(QString::fromUtf8(kKeys[8 + i].name));
    if (names.isEmpty())
        return QString::fromUtf8("（无按键）");
    return names.join(QString::fromUtf8(" + "));
}

} /* namespace Proto */

#endif /* PROTOCOL_H */
