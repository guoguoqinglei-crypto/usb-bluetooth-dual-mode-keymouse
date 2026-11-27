#ifndef PERFORMANCE_MONITOR_H
#define PERFORMANCE_MONITOR_H

#include <QWidget>
#include <QVector>
#include <QString>
#include <QtGlobal>

class QLabel;
class QGridLayout;
class QPushButton;
class RateHistoryChart;

/* 两个上位机共用的性能采样与报告模块。
 * 输入由真实 HID 报告驱动，界面定时器只负责低频刷新，不在读线程中操作控件。 */
class PerformanceMonitor : public QWidget
{
    Q_OBJECT
public:
    explicit PerformanceMonitor(QWidget *parent = nullptr);

    void setSequenceSupport(bool supported) { m_sequenceSupported = supported; }
    void setConnectionState(bool connected);
    void recordInput(qint64 nowMs, bool valid, quint64 sequenceLost = 0);
    void recordFeature(bool write, bool success);
    void resetSession(quint64 outputTotal = 0);
    void stopSession();
    void tick(qint64 nowMs, quint64 outputTotal);

    QString reportText() const;
    QString reportHtml(const QString &project, const QString &device,
                       const QString &link, const QString &functionalReport) const;

signals:
    void resetRequested();
    void exportRequested();

private:
    QLabel *addMetric(QGridLayout *layout, int row, int column, const QString &name);
    void updateUi(qint64 nowMs);
    double averageInterval() const;
    double intervalDeviation() const;
    double averageRate(qint64 nowMs) const;
    quint64 sessionOutput() const;
    QString durationText(qint64 nowMs) const;

    bool m_running = false;
    bool m_connected = false;
    bool m_sequenceSupported = false;
    qint64 m_startedMs = 0;
    qint64 m_stoppedMs = 0;
    qint64 m_windowStartMs = 0;
    qint64 m_lastInputMs = 0;
    qint64 m_lastIntervalMs = 0;
    qint64 m_minIntervalMs = -1;
    qint64 m_maxIntervalMs = 0;

    quint64 m_frames = 0;
    quint64 m_invalidFrames = 0;
    quint64 m_sequenceLost = 0;
    quint64 m_windowFrames = 0;
    quint64 m_intervalCount = 0;
    quint64 m_intervalOver30 = 0;
    quint64 m_stallsOver500 = 0;
    double m_intervalSum = 0.0;
    double m_intervalSquareSum = 0.0;
    double m_rateHz = 0.0;

    quint64 m_outputTotal = 0;
    quint64 m_outputBaseline = 0;
    quint64 m_stoppedOutput = 0;
    quint64 m_featureReadOk = 0;
    quint64 m_featureReadFail = 0;
    quint64 m_featureWriteOk = 0;
    quint64 m_featureWriteFail = 0;
    quint64 m_connections = 0;
    quint64 m_reconnects = 0;

    QVector<double> m_rateHistory;
    RateHistoryChart *m_chart = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_sessionState = nullptr;
    QLabel *m_durationValue = nullptr;
    QLabel *m_framesValue = nullptr;
    QLabel *m_rateValue = nullptr;
    QLabel *m_averageRateValue = nullptr;
    QLabel *m_lastIntervalValue = nullptr;
    QLabel *m_averageIntervalValue = nullptr;
    QLabel *m_jitterValue = nullptr;
    QLabel *m_maxIntervalValue = nullptr;
    QLabel *m_over30Value = nullptr;
    QLabel *m_stallsValue = nullptr;
    QLabel *m_lossValue = nullptr;
    QLabel *m_invalidValue = nullptr;
    QLabel *m_connectionsValue = nullptr;
    QLabel *m_outputValue = nullptr;
    QLabel *m_featureValue = nullptr;
};

#endif /* PERFORMANCE_MONITOR_H */
