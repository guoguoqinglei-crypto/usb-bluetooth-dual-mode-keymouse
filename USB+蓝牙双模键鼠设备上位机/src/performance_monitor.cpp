#include "performance_monitor.h"

#include <QDateTime>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <cmath>

/* 轻量速率曲线：只保留最近 120 个一秒窗口，不引入 Qt Charts。 */
class RateHistoryChart : public QWidget
{
public:
    explicit RateHistoryChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(280);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setSamples(const QVector<double> &samples)
    {
        m_samples = samples;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillRect(rect(), QColor(0xFA, 0xFB, 0xFD));

        const QRectF plot = QRectF(rect()).adjusted(48, 20, -16, -30);
        double maximum = 10.0;
        for (double value : m_samples) maximum = std::max(maximum, value);
        maximum = std::ceil(maximum / 10.0) * 10.0;

        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 9));
        for (int i = 0; i <= 4; ++i) {
            const double y = plot.bottom() - plot.height() * i / 4.0;
            painter.setPen(QPen(QColor(0xE3, 0xE8, 0xEE), 1, Qt::DashLine));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(QColor(0x72, 0x7D, 0x8A));
            painter.drawText(QRectF(0, y - 9, 42, 18), Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(maximum * i / 4.0, 'f', 0));
        }

        painter.setPen(QColor(0x62, 0x70, 0x7E));
        painter.drawText(QRectF(plot.left(), 0, plot.width(), 18),
                         Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("输入报告速率 / Hz"));
        painter.drawText(QRectF(plot.left(), height() - 24, plot.width(), 20),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("最近 %1 个一秒窗口（最多 120 秒）").arg(m_samples.size()));

        if (m_samples.isEmpty()) {
            painter.setPen(QColor(0x8A, 0x94, 0xA0));
            painter.drawText(plot, Qt::AlignCenter, QStringLiteral("开始测试后显示真实 HID 数据"));
            return;
        }

        QPolygonF line;
        line.reserve(m_samples.size());
        const double denominator = std::max<qsizetype>(1, m_samples.size() - 1);
        for (int i = 0; i < m_samples.size(); ++i) {
            const double x = plot.left() + plot.width() * i / denominator;
            const double y = plot.bottom() - plot.height() * m_samples.at(i) / maximum;
            line.append(QPointF(x, y));
        }
        painter.setPen(QPen(QColor(0x16, 0x77, 0xB8), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolyline(line);
        if (line.size() == 1) {
            painter.setBrush(QColor(0x16, 0x77, 0xB8));
            painter.drawEllipse(line.first(), 3, 3);
        }
    }

private:
    QVector<double> m_samples;
};

PerformanceMonitor::PerformanceMonitor(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(700);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(10);

    auto *header = new QHBoxLayout;
    m_sessionState = new QLabel(QStringLiteral("尚未开始"), this);
    m_sessionState->setStyleSheet(QStringLiteral("font-size:16px;font-weight:bold;color:#486581;"));
    header->addWidget(m_sessionState);
    header->addStretch(1);
    m_startButton = new QPushButton(QStringLiteral("开始 / 重新测试"), this);
    m_startButton->setProperty("primary", true);
    m_stopButton = new QPushButton(QStringLiteral("结束测试"), this);
    auto *exportButton = new QPushButton(QStringLiteral("导出测试报告"), this);
    header->addWidget(m_startButton);
    header->addWidget(m_stopButton);
    header->addWidget(exportButton);
    root->addLayout(header);

    auto *explanation = new QLabel(
        QStringLiteral("统计仅来自当前测试期间的真实 HID 数据。间隔异常与断流不等于物理丢包；"
                       "Feature 成功仅表示传输完成，Flash 是否保存成功以配置页的设备状态回读为准。"), this);
    explanation->setWordWrap(true);
    explanation->setStyleSheet(QStringLiteral("color:#627D98;background:#F2F6FA;padding:8px;border-radius:4px;"));
    root->addWidget(explanation);

    auto *metricsBox = new QGroupBox(QStringLiteral("本次测试指标"), this);
    auto *metrics = new QGridLayout(metricsBox);
    metrics->setHorizontalSpacing(14);
    metrics->setVerticalSpacing(9);
    m_durationValue = addMetric(metrics, 0, 0, QStringLiteral("测试时长"));
    m_framesValue = addMetric(metrics, 0, 1, QStringLiteral("有效输入报告"));
    m_invalidValue = addMetric(metrics, 0, 2, QStringLiteral("无效 / 解析失败"));
    m_connectionsValue = addMetric(metrics, 0, 3, QStringLiteral("连接 / 回连"));
    m_rateValue = addMetric(metrics, 1, 0, QStringLiteral("当前速率"));
    m_averageRateValue = addMetric(metrics, 1, 1, QStringLiteral("全程平均速率"));
    m_outputValue = addMetric(metrics, 1, 2, QStringLiteral("输出报告成功"));
    m_featureValue = addMetric(metrics, 1, 3, QStringLiteral("Feature 读 / 写"));
    m_lastIntervalValue = addMetric(metrics, 2, 0, QStringLiteral("最近报告间隔"));
    m_averageIntervalValue = addMetric(metrics, 2, 1, QStringLiteral("平均报告间隔"));
    m_jitterValue = addMetric(metrics, 2, 2, QStringLiteral("间隔标准差（抖动）"));
    m_maxIntervalValue = addMetric(metrics, 2, 3, QStringLiteral("最大报告间隔"));
    m_over30Value = addMetric(metrics, 3, 0, QStringLiteral("间隔 > 30 ms"));
    m_stallsValue = addMetric(metrics, 3, 1, QStringLiteral("断流 > 500 ms"));
    m_lossValue = addMetric(metrics, 3, 2, QStringLiteral("序号丢失估算"));
    root->addWidget(metricsBox);

    m_chart = new RateHistoryChart(this);
    root->addWidget(m_chart, 1);

    connect(m_startButton, &QPushButton::clicked, this, &PerformanceMonitor::resetRequested);
    connect(m_stopButton, &QPushButton::clicked, this, &PerformanceMonitor::stopSession);
    connect(exportButton, &QPushButton::clicked, this, &PerformanceMonitor::exportRequested);
    updateUi(QDateTime::currentMSecsSinceEpoch());
}

QLabel *PerformanceMonitor::addMetric(QGridLayout *layout, int row, int column, const QString &name)
{
    auto *nameLabel = new QLabel(name, this);
    nameLabel->setMinimumHeight(24);
    nameLabel->setStyleSheet(QStringLiteral("color:#52677C;font-size:13px;"));
    auto *valueLabel = new QLabel(QStringLiteral("—"), this);
    valueLabel->setMinimumHeight(32);
    valueLabel->setStyleSheet(QStringLiteral("color:#102A43;font-size:18px;font-weight:bold;"));
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    const int base = column * 2;
    layout->addWidget(nameLabel, row * 2, base);
    layout->addWidget(valueLabel, row * 2 + 1, base);
    layout->setColumnStretch(base, 1);
    layout->setColumnMinimumWidth(base + 1, 10);
    return valueLabel;
}

void PerformanceMonitor::setConnectionState(bool connected)
{
    if (m_running && connected != m_connected) {
        if (connected) {
            if (m_connections == 0) ++m_connections;
            else ++m_reconnects;
            ++m_connections;
        }
        m_lastInputMs = 0;
    }
    m_connected = connected;
    updateUi(QDateTime::currentMSecsSinceEpoch());
}

void PerformanceMonitor::recordInput(qint64 nowMs, bool valid, quint64 sequenceLost)
{
    if (!m_running) return;
    if (!valid) {
        ++m_invalidFrames;
        return;
    }

    ++m_frames;
    ++m_windowFrames;
    m_sequenceLost += sequenceLost;
    if (m_lastInputMs > 0) {
        const qint64 interval = std::max<qint64>(0, nowMs - m_lastInputMs);
        m_lastIntervalMs = interval;
        if (m_minIntervalMs < 0 || interval < m_minIntervalMs) m_minIntervalMs = interval;
        m_maxIntervalMs = std::max(m_maxIntervalMs, interval);
        ++m_intervalCount;
        m_intervalSum += double(interval);
        m_intervalSquareSum += double(interval) * double(interval);
        if (interval > 30) ++m_intervalOver30;
        if (interval > 500) ++m_stallsOver500;
    }
    m_lastInputMs = nowMs;
}

void PerformanceMonitor::recordFeature(bool write, bool success)
{
    if (!m_running) return;
    if (write) {
        if (success) ++m_featureWriteOk;
        else ++m_featureWriteFail;
    } else {
        if (success) ++m_featureReadOk;
        else ++m_featureReadFail;
    }
}

void PerformanceMonitor::resetSession(quint64 outputTotal)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_running = true;
    m_startedMs = now;
    m_stoppedMs = 0;
    m_windowStartMs = now;
    m_lastInputMs = 0;
    m_lastIntervalMs = 0;
    m_minIntervalMs = -1;
    m_maxIntervalMs = 0;
    m_frames = 0;
    m_invalidFrames = 0;
    m_sequenceLost = 0;
    m_windowFrames = 0;
    m_intervalCount = 0;
    m_intervalOver30 = 0;
    m_stallsOver500 = 0;
    m_intervalSum = 0.0;
    m_intervalSquareSum = 0.0;
    m_rateHz = 0.0;
    m_outputTotal = outputTotal;
    m_outputBaseline = outputTotal;
    m_stoppedOutput = 0;
    m_featureReadOk = 0;
    m_featureReadFail = 0;
    m_featureWriteOk = 0;
    m_featureWriteFail = 0;
    m_connections = m_connected ? 1 : 0;
    m_reconnects = 0;
    m_rateHistory.clear();
    m_chart->setSamples(m_rateHistory);
    updateUi(now);
}

void PerformanceMonitor::stopSession()
{
    if (!m_running) return;
    m_stoppedOutput = sessionOutput();
    m_stoppedMs = QDateTime::currentMSecsSinceEpoch();
    m_running = false;
    updateUi(m_stoppedMs);
}

void PerformanceMonitor::tick(qint64 nowMs, quint64 outputTotal)
{
    if (m_running) {
        m_outputTotal = outputTotal;
        const qint64 span = nowMs - m_windowStartMs;
        if (span >= 1000) {
            m_rateHz = double(m_windowFrames) * 1000.0 / double(span);
            m_rateHistory.append(m_rateHz);
            if (m_rateHistory.size() > 120) m_rateHistory.removeFirst();
            m_windowStartMs = nowMs;
            m_windowFrames = 0;
            m_chart->setSamples(m_rateHistory);
        }
    }
    updateUi(nowMs);
}

double PerformanceMonitor::averageInterval() const
{
    return m_intervalCount ? m_intervalSum / double(m_intervalCount) : 0.0;
}

double PerformanceMonitor::intervalDeviation() const
{
    if (!m_intervalCount) return 0.0;
    const double mean = averageInterval();
    return std::sqrt(std::max(0.0, m_intervalSquareSum / double(m_intervalCount) - mean * mean));
}

double PerformanceMonitor::averageRate(qint64 nowMs) const
{
    if (!m_startedMs) return 0.0;
    const qint64 end = m_stoppedMs ? m_stoppedMs : nowMs;
    const qint64 elapsed = std::max<qint64>(0, end - m_startedMs);
    return elapsed ? double(m_frames) * 1000.0 / double(elapsed) : 0.0;
}

quint64 PerformanceMonitor::sessionOutput() const
{
    if (!m_running && m_stoppedMs) return m_stoppedOutput;
    return m_outputTotal >= m_outputBaseline ? m_outputTotal - m_outputBaseline : 0;
}

QString PerformanceMonitor::durationText(qint64 nowMs) const
{
    if (!m_startedMs) return QStringLiteral("—");
    const qint64 end = m_stoppedMs ? m_stoppedMs : nowMs;
    qint64 seconds = std::max<qint64>(0, end - m_startedMs) / 1000;
    const qint64 hours = seconds / 3600;
    seconds %= 3600;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(seconds / 60, 2, 10, QChar('0'))
        .arg(seconds % 60, 2, 10, QChar('0'));
}

void PerformanceMonitor::updateUi(qint64 nowMs)
{
    m_startButton->setEnabled(!m_running);
    m_stopButton->setEnabled(m_running);
    m_sessionState->setText(!m_startedMs ? QStringLiteral("尚未开始")
                            : (m_running ? QStringLiteral("正在采集 · %1").arg(m_connected ? QStringLiteral("设备已连接") : QStringLiteral("等待设备"))
                                         : QStringLiteral("测试已结束 · 统计已冻结")));
    m_durationValue->setText(durationText(nowMs));
    m_framesValue->setText(QStringLiteral("%1 帧").arg(m_frames));
    m_invalidValue->setText(QStringLiteral("%1 帧").arg(m_invalidFrames));
    m_connectionsValue->setText(QStringLiteral("%1 / %2 次").arg(m_connections).arg(m_reconnects));
    m_rateValue->setText(QStringLiteral("%1 Hz").arg(m_rateHz, 0, 'f', 1));
    m_averageRateValue->setText(QStringLiteral("%1 Hz").arg(averageRate(nowMs), 0, 'f', 1));
    m_outputValue->setText(QStringLiteral("%1 帧").arg(sessionOutput()));
    m_featureValue->setText(QStringLiteral("读 %1/%2 · 写 %3/%4")
        .arg(m_featureReadOk).arg(m_featureReadFail).arg(m_featureWriteOk).arg(m_featureWriteFail));
    m_lastIntervalValue->setText(m_intervalCount ? QStringLiteral("%1 ms").arg(m_lastIntervalMs) : QStringLiteral("—"));
    m_averageIntervalValue->setText(m_intervalCount ? QStringLiteral("%1 ms").arg(averageInterval(), 0, 'f', 2) : QStringLiteral("—"));
    m_jitterValue->setText(m_intervalCount ? QStringLiteral("%1 ms").arg(intervalDeviation(), 0, 'f', 2) : QStringLiteral("—"));
    m_maxIntervalValue->setText(m_intervalCount ? QStringLiteral("%1 ms").arg(m_maxIntervalMs) : QStringLiteral("—"));
    m_over30Value->setText(QStringLiteral("%1 次").arg(m_intervalOver30));
    m_stallsValue->setText(QStringLiteral("%1 次").arg(m_stallsOver500));
    m_lossValue->setText(m_sequenceSupported ? QStringLiteral("%1 帧").arg(m_sequenceLost)
                                             : QStringLiteral("无序号，不估算"));
}

QString PerformanceMonitor::reportText() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString text;
    QTextStream out(&text);
    out << "\n性能测试摘要\n"
        << "测试状态：" << (!m_startedMs ? QStringLiteral("未开始") : (m_running ? QStringLiteral("进行中") : QStringLiteral("已结束"))) << "\n"
        << "测试时长：" << durationText(now) << "\n"
        << "有效输入报告：" << m_frames << " 帧；无效／解析失败：" << m_invalidFrames << " 帧\n"
        << "当前速率：" << QString::number(m_rateHz, 'f', 1) << " Hz；全程平均速率："
        << QString::number(averageRate(now), 'f', 1) << " Hz\n"
        << "最近／平均／最大间隔：" << (m_intervalCount ? QString::number(m_lastIntervalMs) : QStringLiteral("—")) << " / "
        << (m_intervalCount ? QString::number(averageInterval(), 'f', 2) : QStringLiteral("—")) << " / "
        << (m_intervalCount ? QString::number(m_maxIntervalMs) : QStringLiteral("—")) << " ms\n"
        << "间隔标准差：" << QString::number(intervalDeviation(), 'f', 2) << " ms\n"
        << "间隔 >30ms：" << m_intervalOver30 << " 次；相邻报告断流 >500ms：" << m_stallsOver500 << " 次\n"
        << "序号丢失估算：" << (m_sequenceSupported ? QStringLiteral("%1 帧").arg(m_sequenceLost) : QStringLiteral("输入报告无序号，不估算")) << "\n"
        << "输出报告实际成功：" << sessionOutput() << " 帧\n"
        << "Feature 读取成功／失败：" << m_featureReadOk << " / " << m_featureReadFail
        << "；写入成功／失败：" << m_featureWriteOk << " / " << m_featureWriteFail << "\n"
        << "连接建立：" << m_connections << " 次；其中回连：" << m_reconnects << " 次\n"
        << "统计口径：报告间隔异常只能提示链路或调度问题，不能直接认定为物理丢包；Feature 成功不等同于 Flash 保存成功。\n";
    return text;
}

QString PerformanceMonitor::reportHtml(const QString &project, const QString &device,
                                       const QString &link, const QString &functionalReport) const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString status = !m_startedMs ? QStringLiteral("未开始") : (m_running ? QStringLiteral("进行中") : QStringLiteral("已结束"));
    const QString started = m_startedMs ? QDateTime::fromMSecsSinceEpoch(m_startedMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) : QStringLiteral("—");

    auto row = [](const QString &name, const QString &value) {
        return QStringLiteral("<tr><th>%1</th><td>%2</td></tr>").arg(name.toHtmlEscaped(), value.toHtmlEscaped());
    };
    QString rows;
    rows += row(QStringLiteral("测试状态"), status);
    rows += row(QStringLiteral("开始时间"), started);
    rows += row(QStringLiteral("测试时长"), durationText(now));
    rows += row(QStringLiteral("有效 / 无效输入报告"), QStringLiteral("%1 / %2 帧").arg(m_frames).arg(m_invalidFrames));
    rows += row(QStringLiteral("当前 / 全程平均速率"), QStringLiteral("%1 / %2 Hz").arg(m_rateHz, 0, 'f', 1).arg(averageRate(now), 0, 'f', 1));
    rows += row(QStringLiteral("最近 / 平均 / 最大间隔"), m_intervalCount
                ? QStringLiteral("%1 / %2 / %3 ms").arg(m_lastIntervalMs).arg(averageInterval(), 0, 'f', 2).arg(m_maxIntervalMs)
                : QStringLiteral("—"));
    rows += row(QStringLiteral("间隔标准差（抖动）"), QStringLiteral("%1 ms").arg(intervalDeviation(), 0, 'f', 2));
    rows += row(QStringLiteral("间隔 >30ms / 断流 >500ms"), QStringLiteral("%1 / %2 次").arg(m_intervalOver30).arg(m_stallsOver500));
    rows += row(QStringLiteral("序号丢失估算"), m_sequenceSupported ? QStringLiteral("%1 帧").arg(m_sequenceLost) : QStringLiteral("无序号，不估算"));
    rows += row(QStringLiteral("输出报告实际成功"), QStringLiteral("%1 帧").arg(sessionOutput()));
    rows += row(QStringLiteral("Feature 读成功 / 失败"), QStringLiteral("%1 / %2").arg(m_featureReadOk).arg(m_featureReadFail));
    rows += row(QStringLiteral("Feature 写成功 / 失败"), QStringLiteral("%1 / %2").arg(m_featureWriteOk).arg(m_featureWriteFail));
    rows += row(QStringLiteral("连接 / 其中回连"), QStringLiteral("%1 / %2 次").arg(m_connections).arg(m_reconnects));

    QString svg = QStringLiteral("<svg viewBox=\"0 0 960 240\" role=\"img\" aria-label=\"输入报告速率曲线\"><rect width=\"960\" height=\"240\" fill=\"#fafbfd\"/>");
    double maximum = 10.0;
    for (double value : m_rateHistory) maximum = std::max(maximum, value);
    maximum = std::ceil(maximum / 10.0) * 10.0;
    for (int i = 0; i <= 4; ++i) {
        const double y = 200 - i * 40;
        svg += QStringLiteral("<line x1=\"48\" y1=\"%1\" x2=\"940\" y2=\"%1\" stroke=\"#dfe6ed\" stroke-dasharray=\"4 4\"/><text x=\"40\" y=\"%2\" text-anchor=\"end\" font-size=\"11\" fill=\"#627d98\">%3</text>")
            .arg(y).arg(y + 4).arg(maximum * i / 4.0, 0, 'f', 0);
    }
    if (m_rateHistory.isEmpty()) {
        svg += QStringLiteral("<text x=\"480\" y=\"120\" text-anchor=\"middle\" fill=\"#7b8794\">没有速率采样，请先开始测试</text>");
    } else {
        QString points;
        const double denominator = std::max<qsizetype>(1, m_rateHistory.size() - 1);
        for (int i = 0; i < m_rateHistory.size(); ++i) {
            const double x = 48.0 + 892.0 * i / denominator;
            const double y = 200.0 - 160.0 * m_rateHistory.at(i) / maximum;
            points += QStringLiteral("%1,%2 ").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2);
        }
        svg += QStringLiteral("<polyline points=\"%1\" fill=\"none\" stroke=\"#1677b8\" stroke-width=\"2\"/>").arg(points);
    }
    svg += QStringLiteral("<text x=\"940\" y=\"226\" text-anchor=\"end\" font-size=\"11\" fill=\"#627d98\">最近 %1 个一秒窗口</text></svg>").arg(m_rateHistory.size());

    QString html;
    QTextStream out(&html);
    out << "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        << "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        << "<title>" << project.toHtmlEscaped() << " 性能测试报告</title>"
        << "<style>body{font-family:'Microsoft YaHei',Arial,sans-serif;max-width:1100px;margin:32px auto;padding:0 20px;color:#243b53;line-height:1.65}"
        << "h1{border-bottom:3px solid #1677b8;padding-bottom:12px}h2{margin-top:28px;color:#334e68}"
        << ".meta{color:#627d98}.notice{background:#fff7e6;border-left:4px solid #d98700;padding:12px 16px}"
        << "table{width:100%;border-collapse:collapse}th,td{border:1px solid #d9e2ec;padding:8px 12px;text-align:left}th{width:32%;background:#f0f4f8}"
        << "svg{width:100%;height:auto;border:1px solid #e3e8ee}pre{white-space:pre-wrap;background:#f5f7fa;padding:16px;border:1px solid #d9e2ec;font-family:Consolas,'Microsoft YaHei',monospace}</style></head><body>"
        << "<h1>" << project.toHtmlEscaped() << " · 性能测试报告</h1>"
        << "<p class=\"meta\">生成时间：" << QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
        << "<br>设备：" << device.toHtmlEscaped() << "<br>链路：" << link.toHtmlEscaped() << "</p>"
        << "<div class=\"notice\">本报告由上位机根据真实 HID 数据生成。报告间隔异常和断流不能直接认定为物理丢包；Feature 传输成功不单独证明 Flash 已保存。本报告不替代实物功能验收。</div>"
        << "<h2>一、实测指标</h2><table>" << rows << "</table>"
        << "<h2>二、输入速率曲线</h2>" << svg
        << "<h2>三、统计口径</h2><ul>"
        << "<li>当前速率按最近一个完整的一秒窗口计算；平均速率使用本次测试总时长。</li>"
        << "<li>抖动为有效输入报告间隔的总体标准差；断流指相邻有效报告间隔超过 500 ms。</li>"
        << "<li>只有带报告序号的键鼠协议提供序号丢失估算；手柄报告无序号，因此不计算物理丢包。</li>"
        << "<li>输出计数来自 HID 写线程实际成功确认，不是按钮点击次数或入队次数。</li></ul>"
        << "<h2>四、功能验收原始记录</h2><pre>" << functionalReport.toHtmlEscaped() << "</pre>"
        << "</body></html>";
    return html;
}
