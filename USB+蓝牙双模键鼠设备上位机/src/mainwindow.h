#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QWidget>
#include <QByteArray>
#include <QList>
#include <QVector>
#include <QTimer>
#include <QElapsedTimer>
#include <QColor>
#include <QStringList>
#include "hiddevice.h"

class QLabel; class QPushButton; class QComboBox; class QSlider; class QSpinBox;
class QCheckBox; class QPlainTextEdit; class QLineEdit; class QGroupBox;

class KeyIndicator : public QWidget
{
public:
    KeyIndicator(const QString &name, const QString &func,
                 const QColor &color, QWidget *parent = nullptr);
    void setDown(bool down);
    void setPressCount(int n);
    void resetCount();
    bool isDown() const { return m_down; }
    int pressCount() const { return m_count; }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QString m_name;
    QString m_func;
    QColor  m_color;
    bool    m_down = false;
    int     m_count = 0;
};

class ColorSwatch : public QWidget
{
public:
    explicit ColorSwatch(QWidget *parent = nullptr);
    void setColor(int r, int g, int b);
    QColor color() const { return m_color; }
protected:
    void paintEvent(QPaintEvent *event) override;
private:
    QColor m_color = QColor(0, 0, 0);
    int    m_pct[3] = { 0, 0, 0 };
};

namespace Dashboard { class MetricCard; class AssessmentBar; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
private slots:
    void onRefreshDevices();
    void onConnectClicked();
    void onReport(const QByteArray &raw);
    void onDeviceLost(const QString &reason);
    void onLog(const QString &text);
    void onSendRgb();
    void onPresetClicked();
    void onSliderChanged(int value);
    void onRainbowTick();
    void onAutoReconnectTick();
    void onHeartbeatTick();
    void onSendRaw();
    void onClearLog();
    void onSaveLog();
    void onClearCounters();
    void onReadStatus();
    void onShowAssessment();
    void onExportReport();
    void onStartRgbTest();
    void onRgbTestTick();
    /* 节流相关：把高频操作合并到低频定时器里执行 */
    void onSendTick();
    void onUiTick();
    void onFlushLog();
private:
    void buildUi();
    void setConnected(bool on);
    void populateDeviceCombo();
    void appendLog(const QString &tag, const QString &text, const QColor &color = QColor());
    void updateStats();
    void updateAssessment();
    QString assessmentReport() const;
    void updateLockBadges();
    void refreshUi();
    void applyColor(int r, int g, int b, bool send);
    void queueRgb(int r, int g, int b);
    void sendRgbNow(int r, int g, int b);
    void sendRainbowMode(bool on);
    void loadSettings();
    void saveSettings();

    HidDevice m_hid;
    QList<HidDeviceInfo> m_devices;

    QComboBox *m_deviceCombo = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QLabel *m_connLabel = nullptr;
    QCheckBox *m_autoReconnect = nullptr;
    QCheckBox *m_liveSend = nullptr;
    QCheckBox *m_rainbow = nullptr;
    QCheckBox *m_pauseLog = nullptr;
    QCheckBox *m_logEveryFrame = nullptr;
    ColorSwatch *m_colorPreview = nullptr;
    QLabel *m_heartbeatLabel = nullptr;
    QPlainTextEdit *m_logView = nullptr;
    QLineEdit *m_rawEdit = nullptr;
    QGroupBox *m_rgbBox = nullptr;
    Dashboard::MetricCard *m_framesCard = nullptr;
    Dashboard::MetricCard *m_keysCard = nullptr;
    Dashboard::MetricCard *m_lossCard = nullptr;
    Dashboard::MetricCard *m_rgbCard = nullptr;
    Dashboard::AssessmentBar *m_assessmentBar = nullptr;

    QVector<KeyIndicator *> m_keys;
    QVector<QSlider *> m_sliders;
    QVector<QSpinBox *> m_spinBoxes;
    QLabel *m_bmLabel = nullptr;
    QLabel *m_seqLabel = nullptr;
    QLabel *m_frameLabel = nullptr;
    QLabel *m_lostLabel = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_capsLabel = nullptr;
    QLabel *m_numLabel = nullptr;
    QLabel *m_eventLabel = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_modeLabel = nullptr;       /* 工作模式（自动 / 强制蓝牙） */

    QTimer m_reconnectTimer;
    QTimer m_rainbowTimer;
    QTimer m_rgbTestTimer;
    QTimer m_heartbeatTimer;
    QTimer m_sendTimer;      /* 合并 RGB 下发，40ms */
    QTimer m_uiTimer;        /* 按键面板文本刷新，50ms */
    QTimer m_logTimer;       /* 日志批量 flush，80ms */
    QElapsedTimer m_connTime;

    quint8  m_lastBitmap = 0;
    quint8  m_lastStickBitmap = 0;       /* rep[8]：L3 / R3 */
    quint8  m_lastStatus = 0;            /* 设备上报的状态字节 rep[3] */
    quint8  m_diagReason = 0xFF;         /* 诊断：最近断开原因码 rep[4] */
    quint8  m_diagTries  = 0;            /* 诊断：回连尝试次数 rep[5] */
    quint8  m_diagState  = 0;            /* 诊断：蓝牙状态机状态 rep[6] */
    quint8  m_diagChan   = 0xFF;         /* 诊断：HID 通道状态 rep[7]（bit0控制 bit1中断 bit2可发） */
    quint8  m_ledState = 0;
    quint8  m_testedKeys = 0;
    quint8  m_testedSticks = 0;
    quint8  m_seenLedState = 0;
    int     m_lastSeq = -1;
    quint64 m_frames = 0;
    quint64 m_lostFrames = 0;
    quint64 m_rgbBaseline = 0;
    quint64 m_lastFrameMs = 0;
    int     m_rainbowHue = 0;
    int     m_rgbTestStep = 0;
    QStringList m_events;
    QStringList m_logBuf;
    int     m_curRgb[3] = { 0, 0, 0 };
    QColor  m_swatchColor = QColor(0, 0, 0);
    qint64  m_lastOutLogMs = 0;
    qint64  m_lastFrameLogMs = 0;
    bool    m_rgbDirty = false;
    bool    m_uiDirty = false;
    bool    m_connectWanted = false;
    bool    m_connected = false;
    QString m_reconnectPath;
};

#endif /* MAINWINDOW_H */
