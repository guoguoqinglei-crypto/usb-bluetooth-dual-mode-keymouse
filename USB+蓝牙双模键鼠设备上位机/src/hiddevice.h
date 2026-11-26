#ifndef HIDDEVICE_H
#define HIDDEVICE_H

/* Windows native HID layer (SetupAPI + hid.dll), no third-party dependency.
 * windows.h is intentionally NOT included here to avoid min/max macros;
 * OS handles are stored as void* and cast inside the .cpp.
 *
 * Threading: one reader thread (blocking overlapped ReadFile) and one writer
 * thread.  sendOutputReport() only enqueues, so the GUI thread never blocks
 * inside a USB transfer. */

#include <QObject>
#include <QString>
#include <QByteArray>
#include <QList>
#include <QMutex>
#include <atomic>

enum class HidTransport { Usb, Bluetooth };

struct HidDeviceInfo
{
    QString path;
    QString product;
    QString serial;
    quint16 vid = 0;
    quint16 pid = 0;
    quint16 usagePage = 0;
    quint16 usage = 0;
    quint16 inputReportLen = 0;
    quint16 outputReportLen = 0;
    HidTransport transport = HidTransport::Usb;

    bool isValid() const { return !path.isEmpty(); }
    bool isBluetooth() const { return transport == HidTransport::Bluetooth; }
    quint8 reportId() const { return isBluetooth() ? quint8(3) : quint8(0); }
    QString transportName() const { return isBluetooth() ? QStringLiteral("蓝牙") : QStringLiteral("USB"); }
    QString label() const;
};

class HidDevice : public QObject
{
    Q_OBJECT
public:
    explicit HidDevice(QObject *parent = nullptr);
    ~HidDevice() override;

    static QList<HidDeviceInfo> enumerate();

    bool open(const HidDeviceInfo &info);
    void close();
    bool isOpen() const { return m_handle != nullptr; }
    const HidDeviceInfo &info() const { return m_info; }

    /* Non-blocking: frame is queued and sent by the writer thread.
     * Frame length should be 9 (Report ID byte first); it is padded/truncated
     * to the reported output length before the transfer.
     * A pending SET_RGB frame is overwritten instead of queued twice, so a
     * slider drag cannot flood the endpoint. */
    bool sendOutputReport(const QByteArray &frame);

    int  pendingWrites();

    /* 只统计写线程中真正成功的 HID 输出报告，供考核记录使用。 */
    quint64 outOkCount() const { return m_outOk.load(std::memory_order_relaxed); }

signals:
    void reportReceived(const QByteArray &raw);
    void disconnected(const QString &reason);
    void logLine(const QString &text);

private:
    void readLoop();
    void writeLoop();
    bool writeRaw(const QByteArray &frame);

    void *m_handle = nullptr;
    void *m_stopEvent = nullptr;
    void *m_thread = nullptr;
    void *m_writeEvent = nullptr;
    void *m_writerThread = nullptr;
    HidDeviceInfo m_info;
    volatile bool m_running = false;

    QMutex m_queueMutex;
    QList<QByteArray> m_queue;
    int     m_writeMode = 0;   /* 0 = unknown, 1 = SET_REPORT, 2 = interrupt OUT */
    quint64 m_writeErrors = 0;
    std::atomic<quint64> m_outOk{0};
};

#endif
