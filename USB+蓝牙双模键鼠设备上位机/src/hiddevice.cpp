/* Windows 原生 HID 通信层：SetupAPI 枚举 + hid.dll 属性 + 重叠 I/O 读写。
 * 只有本文件包含 windows.h，界面层完全不碰 Win32 类型。 */

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <setupapi.h>
/* MinGW hidsdi.h / hidpi.h have no extern "C" wrapper.
 * Without this, C++ name mangling breaks linkage against libhid.a. */
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include "hiddevice.h"
#include "protocol.h"

#include <thread>
#include <algorithm>

namespace {

/* HID 字符串描述符是 UTF-16LE，长度由设备自己决定。HidD_GetProductString 只保证
 * 写入以 \0 结尾的内容，缓冲区后面的字节可能是残留数据；必须按 NUL 截断，
 * 否则下拉框里会拖出一长串方块字符（整个窗口都会被它撑宽）。 */
QString wideStr(const wchar_t *w, int maxChars)
{
    if (!w) return QString();

    int n = 0;
    while (n < maxChars && w[n] != L'\0') ++n;

    QString raw = QString::fromWCharArray(w, n);
    raw.remove(QChar(QChar::ReplacementCharacter));
    raw.remove(QChar(0xFFFD));

    /* 只保留可打印字符，去掉描述符里带出来的控制符 */
    QString out;
    out.reserve(raw.size());
    for (const QChar ch : raw) {
        if (ch.isPrint()) out.append(ch);
    }
    return out.simplified().trimmed();
}

QString winError(DWORD code)
{
    wchar_t *msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    QString s = msg ? QString::fromWCharArray(msg).trimmed()
                    : QString("Win32 error %1").arg(code);
    if (msg) LocalFree(msg);
    return s;
}

/* 用零权限句柄查询属性，这样不会和系统独占的键盘/鼠标接口冲突 */
struct Query {
    bool ok = false;
    quint16 vid = 0, pid = 0, usagePage = 0, usage = 0;
    quint16 inLen = 0, outLen = 0;
    QString product, serial;
};

Query queryDevice(const QString &path)
{
    Query q;
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t *>(path.utf16()), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return q;

    HIDD_ATTRIBUTES attr;
    attr.Size = sizeof(attr);
    if (HidD_GetAttributes(h, &attr)) {
        q.vid = quint16(attr.VendorID);
        q.pid = quint16(attr.ProductID);
    }

    PHIDP_PREPARSED_DATA ppd = nullptr;
    if (HidD_GetPreparsedData(h, &ppd) && ppd) {
        HIDP_CAPS caps;
        if (HidP_GetCaps(ppd, &caps) == HIDP_STATUS_SUCCESS) {
            q.usagePage = quint16(caps.UsagePage);
            q.usage     = quint16(caps.Usage);
            q.inLen     = quint16(caps.InputReportByteLength);
            q.outLen    = quint16(caps.OutputReportByteLength);
        }
        HidD_FreePreparsedData(ppd);
    }

    wchar_t wbuf[256];
    wmemset(wbuf, 0, 256);
    if (HidD_GetProductString(h, wbuf, sizeof(wbuf)))
        q.product = wideStr(wbuf, 256);
    wmemset(wbuf, 0, 256);
    if (HidD_GetSerialNumberString(h, wbuf, sizeof(wbuf)))
        q.serial = wideStr(wbuf, 256);

    CloseHandle(h);
    q.ok = true;
    return q;
}

} /* namespace */

QString HidDeviceInfo::label() const
{
    QString name = product.isEmpty() ? QStringLiteral("Custom HID") : product;
    /* 产品名可能来自乱七八糟的描述符，限长避免把下拉框/窗口撑宽 */
    if (name.size() > 28) name = name.left(28) + QStringLiteral("...");

    const auto hex4 = [](quint16 v) {
        return QString::number(v, 16).toUpper().rightJustified(4, QChar('0'));
    };
    return QStringLiteral("%1 · %2  [VID:%3 PID:%4  UP:%5 U:%6]")
        .arg(transportName(), name, hex4(vid), hex4(pid), hex4(usagePage), hex4(usage));
}

HidDevice::HidDevice(QObject *parent) : QObject(parent) {}

HidDevice::~HidDevice() { close(); }

QList<HidDeviceInfo> HidDevice::enumerate()
{
    QList<HidDeviceInfo> result;

    GUID guid;
    HidD_GetHidGuid(&guid);

    HDEVINFO infoSet = SetupDiGetClassDevsW(&guid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (infoSet == INVALID_HANDLE_VALUE) return result;

    SP_DEVICE_INTERFACE_DATA iface;
    iface.cbSize = sizeof(iface);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(infoSet, nullptr, &guid, i, &iface); ++i) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(infoSet, &iface, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        QByteArray storage(int(needed), char(0));
        auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(infoSet, &iface, detail, needed, nullptr, nullptr))
            continue;

        QString path = QString::fromWCharArray(detail->DevicePath);
        Query q = queryDevice(path);
        const bool usb = q.vid == Proto::kVid && q.pid == Proto::kPid;
        const bool bluetooth = q.vid == Proto::kBtVid && q.pid == Proto::kBtPid;

        /* 已知 VID/PID + 厂商 Usage 三重限定，不会误开标准键鼠接口。 */
        if (!q.ok || (!usb && !bluetooth)) continue;
        if (q.usagePage != Proto::kUsagePage || q.usage != Proto::kUsage) continue;

        HidDeviceInfo d;
        d.path = path;
        d.product = q.product;
        d.serial = q.serial;
        d.vid = q.vid;
        d.pid = q.pid;
        d.usagePage = q.usagePage;
        d.usage = q.usage;
        d.inputReportLen = q.inLen ? q.inLen : quint16(Proto::kInputXferSize);
        d.outputReportLen = q.outLen ? q.outLen : quint16(Proto::kXferSize);
        d.transport = bluetooth ? HidTransport::Bluetooth : HidTransport::Usb;
        result.append(d);
    }

    SetupDiDestroyDeviceInfoList(infoSet);
    std::sort(result.begin(), result.end(), [](const HidDeviceInfo &a, const HidDeviceInfo &b) {
        if (a.transport != b.transport) return a.transport == HidTransport::Usb;
        return a.path < b.path;
    });
    return result;
}

bool HidDevice::open(const HidDeviceInfo &info)
{
    close();
    if (!info.isValid()) return false;

    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t *>(info.path.utf16()),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        emit logLine(QString("打开设备失败：%1").arg(winError(GetLastError())));
        return false;
    }

    m_handle = h;
    m_info = info;
    m_writeMode = 0;
    m_writeErrors = 0;
    m_outOk.store(0, std::memory_order_relaxed);
    {
        QMutexLocker lock(&m_queueMutex);
        m_queue.clear();
    }
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_writeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_running = true;
    m_thread = new std::thread(&HidDevice::readLoop, this);
    m_writerThread = new std::thread(&HidDevice::writeLoop, this);

    emit logLine(QString("已连接：%1  VID=%2 PID=%3  IN=%4B OUT=%5B  ReportID=%6")
                     .arg(info.transportName())
                     .arg(info.vid, 4, 16, QChar('0')).toUpper()
                     .arg(info.pid, 4, 16, QChar('0')).toUpper()
                     .arg(info.inputReportLen).arg(info.outputReportLen)
                     .arg(info.reportId()));
    return true;
}

void HidDevice::close()
{
    if (!m_handle) return;

    m_running = false;
    if (m_stopEvent) SetEvent(static_cast<HANDLE>(m_stopEvent));
    if (m_writeEvent) SetEvent(static_cast<HANDLE>(m_writeEvent));

    if (m_thread) {
        auto *t = static_cast<std::thread *>(m_thread);
        if (t->joinable()) t->join();
        delete t;
        m_thread = nullptr;
    }
    if (m_writerThread) {
        auto *t = static_cast<std::thread *>(m_writerThread);
        if (t->joinable()) t->join();
        delete t;
        m_writerThread = nullptr;
    }
    if (m_writeEvent) { CloseHandle(static_cast<HANDLE>(m_writeEvent)); m_writeEvent = nullptr; }
    if (m_stopEvent) { CloseHandle(static_cast<HANDLE>(m_stopEvent)); m_stopEvent = nullptr; }
    CloseHandle(static_cast<HANDLE>(m_handle));
    m_handle = nullptr;

    QMutexLocker lock(&m_queueMutex);
    m_queue.clear();
}

void HidDevice::readLoop()
{
    HANDLE h = static_cast<HANDLE>(m_handle);
    HANDLE stop = static_cast<HANDLE>(m_stopEvent);
    const DWORD len = DWORD(m_info.inputReportLen);
    QByteArray buf(int(len), char(0));
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    while (m_running) {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ev;
        ResetEvent(ev);

        DWORD got = 0;
        BOOL ok = ReadFile(h, buf.data(), len, &got, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                if (m_running)
                    emit disconnected(QString("读取失败：%1").arg(winError(err)));
                break;
            }
            HANDLE waits[2] = { ev, stop };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) { CancelIo(h); break; }
            if (w != WAIT_OBJECT_0) break;
            if (!GetOverlappedResult(h, &ov, &got, TRUE)) {
                if (m_running)
                    emit disconnected(QString("读取失败：%1").arg(winError(GetLastError())));
                break;
            }
        }
        if (got > 0) emit reportReceived(QByteArray(buf.constData(), int(got)));
    }

    CloseHandle(ev);
}

int HidDevice::pendingWrites()
{
    QMutexLocker lock(&m_queueMutex);
    return int(m_queue.size());
}

bool HidDevice::sendOutputReport(const QByteArray &frame)
{
    if (!m_handle || frame.size() < 2) return false;

    {
        QMutexLocker lock(&m_queueMutex);
        /* 本协议没有 Report ID，frame[0] 是 0 占位，命令字在 frame[1]。 */
        const bool isRgb = (frame.at(1) == char(Proto::kCmdSetRgb));
        if (isRgb && !m_queue.isEmpty() && m_queue.last().size() > 1 &&
            m_queue.last().at(1) == char(Proto::kCmdSetRgb))
            m_queue.last() = frame;
        else {
            if (m_queue.size() >= 32) m_queue.removeFirst();
            m_queue.append(frame);
        }
    }
    if (m_writeEvent) SetEvent(static_cast<HANDLE>(m_writeEvent));
    return true;
}

/* 写线程：真正的 USB 传输都在这里做，GUI 线程只负责入队 */
void HidDevice::writeLoop()
{
    HANDLE ev = static_cast<HANDLE>(m_writeEvent);

    while (m_running) {
        QByteArray frame;
        {
            QMutexLocker lock(&m_queueMutex);
            if (!m_queue.isEmpty()) frame = m_queue.takeFirst();
        }
        if (frame.isEmpty()) {
            if (ev) WaitForSingleObject(ev, 200);
            else Sleep(20);
            continue;
        }
        if (writeRaw(frame)) {
            m_outOk.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++m_writeErrors;
            /* 失败日志限流：前 3 次 + 之后每 100 次记一条，避免刷屏 */
            if (m_writeErrors <= 3 || (m_writeErrors % 100) == 0)
                emit logLine(QString("发送失败（累计 %1 次）").arg(m_writeErrors));
        }
    }
}

bool HidDevice::writeRaw(const QByteArray &frame)
{
    HANDLE h = static_cast<HANDLE>(m_handle);
    if (!h) return false;

    const ULONG need = ULONG(m_info.outputReportLen ? m_info.outputReportLen : frame.size());
    QByteArray buf = frame;
    if (buf.size() < int(need)) buf.append(QByteArray(int(need) - buf.size(), char(0)));
    if (buf.size() > int(need)) buf.truncate(int(need));

    /* 单次命令使用 SET_REPORT；彩虹渐变由板端执行，不再连续占用 USB。 */
    if (m_writeMode != 2 && HidD_SetOutputReport(h, reinterpret_cast<PVOID>(buf.data()), need)) {
        m_writeMode = 1;
        return true;
    }

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof(ov));
    ov.hEvent = ev;
    DWORD written = 0;
    BOOL ok = WriteFile(h, buf.data(), need, &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        /* 250ms 足够一次中断传输；超时就取消，绝不让写线程长期卡住 */
        if (WaitForSingleObject(ev, 250) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(h, &ov, &written, FALSE);
        else {
            CancelIoEx(h, &ov);
            /* 等取消真正完成后再释放 OVERLAPPED 和事件，避免异步使用已释放对象。 */
            GetOverlappedResult(h, &ov, &written, TRUE);
            ok = FALSE;
        }
    }
    CloseHandle(ev);

    if (ok && written == need) {
        m_writeMode = 2;
        return true;
    }
    return false;
}

