/* ============================================================================
 * 上位机主窗口实现
 *   - 左侧：板载 8 键及 L3/R3 的实时状态、累计次数、事件记录
 *   - 右侧：RGB 三通道滑块 / 预设色 / 下发 SET_RGB（考核 3.2）
 *   - 下方/其它：通信日志、丢帧与心跳统计、自动重连、原始命令、彩虹动画
 * ========================================================================== */

#include "mainwindow.h"
#include "protocol.h"
#include "dashboard.h"
#include "performance_monitor.h"

#include <QApplication>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSlider>
#include <QSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QTabWidget>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QStatusBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QTextStream>
#include <QStringConverter>
#include <QDateTime>
#include <QSettings>
#include <QPainter>
#include <QFont>
#include <QColor>
#include <QSizePolicy>
#include <QFile>
#include <QRegularExpression>
#include <QScrollBar>
#include <QScrollArea>
#include <QFrame>

/* 页内容保持合理高度，窗口不够高时由外层滚动条浏览，避免压缩文字和控件。 */
static QScrollArea *makeScrollPage(QWidget *content, int minimumHeight)
{
    content->setMinimumHeight(minimumHeight);
    auto *area = new QScrollArea;
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setWidget(content);
    return area;
}

/* ---------------------------------------------------------------------------
 * ColorSwatch：自绘色块。用 QLabel + setStyleSheet 改背景时 Qt 每次都要重新
 * 解析样式表并 polish 控件，滑动/彩虹时一秒几十次会把 GUI 线程拖住，这里改成
 * 一个纯 paintEvent 的小控件，代价只有一次 update()。
 * ------------------------------------------------------------------------- */
ColorSwatch::ColorSwatch(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(56);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void ColorSwatch::setColor(int r, int g, int b)
{
    if (m_pct[0] == r && m_pct[1] == g && m_pct[2] == b) return;
    m_pct[0] = r; m_pct[1] = g; m_pct[2] = b;
    QColor c;
    c.setRgbF(r / 100.0, g / 100.0, b / 100.0, 1.0);
    m_color = c;
    update();
}

void ColorSwatch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);

    p.setPen(QPen(QColor(0xD0, 0xD4, 0xD9), 1));
    p.setBrush(m_color);
    p.drawRoundedRect(r, 5, 5);

    /* 亮度决定文字颜色，保证在黑白底上都看得清 */
    const QColor text = (m_color.value() > 140) ? QColor(40, 44, 52) : QColor(240, 242, 246);
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    p.setPen(text);
    p.drawText(rect(), Qt::AlignCenter,
               QStringLiteral("%1   R%2%  G%3%  B%4%")
                   .arg(m_color.name().toUpper())
                   .arg(m_pct[0]).arg(m_pct[1]).arg(m_pct[2]));
}

/* ---------------------------------------------------------------------------
 * KeyIndicator
 * ------------------------------------------------------------------------- */
KeyIndicator::KeyIndicator(const QString &name, const QString &func,
                           const QColor &color, QWidget *parent)
    : QWidget(parent), m_name(name), m_func(func), m_color(color)
{
    setMinimumSize(185, 58);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void KeyIndicator::setDown(bool down)
{
    if (m_down == down) return;
    m_down = down;
    update();
}

void KeyIndicator::setPressCount(int n)
{
    m_count = n;
    update();
}

void KeyIndicator::resetCount()
{
    m_count = 0;
    update();
}

void KeyIndicator::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int h = height();

    QColor lamp = m_down ? m_color : QColor(220, 224, 230);
    if (m_down) {
        QColor glow = m_color;
        glow.setAlpha(60);
        p.setBrush(glow);
        p.setPen(Qt::NoPen);
        p.drawEllipse(4, h / 2 - 15, 30, 30);
    }
    p.setBrush(lamp);
    p.setPen(QPen(m_color.darker(140), 1));
    p.drawEllipse(9, h / 2 - 10, 20, 20);

    /* 名称、说明和次数分区绘制；所有宽度随控件变化，不再固定为 130px。 */
    const QRect nameRect(40, 3, width() - 96, h / 2 - 2);
    const QRect countRect(width() - 50, 3, 46, h / 2 - 2);
    const QRect functionRect(40, h / 2, width() - 50, h / 2 - 3);
    QFont f = font();
    f.setBold(m_down);
    f.setPointSize(10);
    p.setFont(f);
    p.setPen(m_down ? m_color.darker(170) : QColor(48, 55, 65));
    p.drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(f).elidedText(m_name, Qt::ElideRight, nameRect.width()));

    QFont sf = font();
    sf.setPointSize(9);
    p.setFont(sf);
    p.setPen(QColor(96, 105, 116));
    p.drawText(functionRect, Qt::AlignLeft | Qt::AlignVCenter,
               QFontMetrics(sf).elidedText(m_func, Qt::ElideRight, functionRect.width()));

    p.setFont(f);
    p.setPen(m_down ? m_color.darker(150) : QColor(120, 128, 138));
    p.drawText(countRect, Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("x%1").arg(m_count));
}

/* ---------------------------------------------------------------------------
 * MainWindow
 * ------------------------------------------------------------------------- */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_connTime.start();

    /* 三个节流定时器：把高频事件合并到低频执行，GUI 线程不再被 USB 传输拖住。
     * 必须在 buildUi()/loadSettings() 之前配置好，否则滑块初始值会触发未初始化的定时器。 */
    m_sendTimer.setSingleShot(true);
    m_sendTimer.setInterval(40);
    connect(&m_sendTimer, &QTimer::timeout, this, &MainWindow::onSendTick);

    m_uiTimer.setInterval(50);
    connect(&m_uiTimer, &QTimer::timeout, this, &MainWindow::onUiTick);

    m_logTimer.setInterval(80);
    connect(&m_logTimer, &QTimer::timeout, this, &MainWindow::onFlushLog);

    buildUi();
    loadSettings();

    connect(&m_hid, &HidDevice::reportReceived,
            this, &MainWindow::onReport, Qt::QueuedConnection);
    connect(&m_hid, &HidDevice::disconnected,
            this, &MainWindow::onDeviceLost, Qt::QueuedConnection);
    connect(&m_hid, &HidDevice::logLine,
            this, &MainWindow::onLog, Qt::QueuedConnection);
    connect(&m_hid, &HidDevice::featureResult,
            this, &MainWindow::onFeatureResult, Qt::QueuedConnection);
    m_configPollTimer.setSingleShot(true);
    m_configPollTimer.setInterval(100);
    connect(&m_configPollTimer, &QTimer::timeout, this, &MainWindow::onConfigPoll);

    /* 2 秒一次：枚举要遍历全部 HID 接口并逐个打开句柄，1 秒一次在断开状态下
     * 会有周期性的小卡顿 */
    m_reconnectTimer.setInterval(2000);
    connect(&m_reconnectTimer, &QTimer::timeout,
            this, &MainWindow::onAutoReconnectTick);
    m_reconnectTimer.start();

    m_rainbowTimer.setInterval(60);
    connect(&m_rainbowTimer, &QTimer::timeout,
            this, &MainWindow::onRainbowTick);

    m_rgbTestTimer.setInterval(550);
    connect(&m_rgbTestTimer, &QTimer::timeout, this, &MainWindow::onRgbTestTick);

    m_heartbeatTimer.setInterval(500);
    connect(&m_heartbeatTimer, &QTimer::timeout,
            this, &MainWindow::onHeartbeatTick);
    m_heartbeatTimer.start();

    m_uiTimer.start();

    onRefreshDevices();
    appendLog(QStringLiteral("SYS"), QStringLiteral("上位机已启动，点击「刷新设备」后连接"));
}

MainWindow::~MainWindow()
{
    saveSettings();
    m_hid.close();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("USB / 蓝牙键鼠控制台  |  自定义 HID 双向交互"));
    resize(1180, 800);

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("root"));
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(14, 12, 14, 10);
    root->setSpacing(9);
    setCentralWidget(central);

    m_connLabel = new QLabel(QStringLiteral("未连接"), central);
    root->addWidget(Dashboard::makeHero(
        QStringLiteral("USB / 蓝牙键鼠控制台"),
        QStringLiteral("STM32F103 + RDA5876  ·  USB 优先  ·  键盘 + 鼠标 + 自定义 HID"),
        m_connLabel, central));

    /* ---------- 设备连接条 ---------- */
    auto *top = new QGroupBox(QStringLiteral("设备连接"), central);
    auto *topLay = new QHBoxLayout(top);
    m_deviceCombo = new QComboBox(top);
    m_deviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_deviceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_deviceCombo->setMinimumContentsLength(22);
    m_refreshBtn = new QPushButton(QStringLiteral("刷新设备"), top);
    m_connectBtn = new QPushButton(QStringLiteral("连接设备"), top);
    m_connectBtn->setProperty("primary", true);
    m_autoReconnect = new QCheckBox(QStringLiteral("自动重连"), top);
    m_autoReconnect->setChecked(true);
    topLay->addWidget(new QLabel(QStringLiteral("自定义 HID"), top));
    topLay->addWidget(m_deviceCombo, 1);
    topLay->addWidget(m_refreshBtn);
    topLay->addWidget(m_connectBtn);
    topLay->addWidget(m_autoReconnect);
    root->addWidget(top);

    auto *metrics = new QHBoxLayout;
    metrics->setSpacing(9);
    m_framesCard = new Dashboard::MetricCard(QStringLiteral("输入报告"), QStringLiteral("自定义 HID · 帧"), central);
    m_keysCard = new Dashboard::MetricCard(QStringLiteral("按键覆盖"), QStringLiteral("8 个板载按键"), central);
    m_lossCard = new Dashboard::MetricCard(QStringLiteral("序号丢失"), QStringLiteral("按报告序号估算"), central);
    m_rgbCard = new Dashboard::MetricCard(QStringLiteral("RGB 成功下发"), QStringLiteral("写线程实际确认"), central);
    for (QWidget *card : {m_framesCard, m_keysCard, m_lossCard, m_rgbCard}) metrics->addWidget(card, 1);
    root->addLayout(metrics);

    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::onRefreshDevices);
    connect(m_connectBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);

    /* ---------- 主体：左按键 / 右 RGB ---------- */
    auto *body = new QHBoxLayout;
    body->setSpacing(10);

    /* 左：按键状态 */
    auto *keyBox = new QGroupBox(QStringLiteral("板载按键实时状态（自定义 HID 输入报告）"), central);
    auto *keyLay = new QVBoxLayout(keyBox);
    auto *keyGrid = new QGridLayout;
    keyGrid->setSpacing(6);
    static const QColor colors[10] = {
        QColor(0xE4, 0x3B, 0x44), QColor(0xE4, 0x7B, 0x2B),
        QColor(0xE4, 0xC0, 0x2B), QColor(0x4C, 0xAF, 0x50),
        QColor(0x26, 0xA6, 0x9A), QColor(0x3F, 0x7C, 0xE0),
        QColor(0x7E, 0x57, 0xC2), QColor(0x8D, 0x6E, 0x63),
        QColor(0x00, 0x96, 0x88), QColor(0x5C, 0x6B, 0xC0)
    };
    for (int i = 0; i < 10; ++i) {
        auto *ind = new KeyIndicator(
            QString::fromUtf8(Proto::kKeys[i].name),
            QString::fromUtf8(Proto::kKeys[i].function),
            colors[i], keyBox);
        m_keys.append(ind);
        keyGrid->addWidget(ind, i / 2, i % 2);
    }
    keyLay->addLayout(keyGrid);

    auto *statGrid = new QGridLayout;
    statGrid->setSpacing(4);
    m_bmLabel     = new QLabel(QStringLiteral("位图 --"), keyBox);
    m_seqLabel    = new QLabel(QStringLiteral("序号 --"), keyBox);
    m_frameLabel  = new QLabel(QStringLiteral("报告 0 帧"), keyBox);
    m_lostLabel   = new QLabel(QStringLiteral("丢帧 0"), keyBox);
    m_timeLabel   = new QLabel(QStringLiteral("连接 --"), keyBox);
    m_heartbeatLabel = new QLabel(QStringLiteral("心跳 --"), keyBox);
    statGrid->addWidget(m_bmLabel,     0, 0);
    statGrid->addWidget(m_seqLabel,    0, 1);
    statGrid->addWidget(m_frameLabel,  1, 0);
    statGrid->addWidget(m_lostLabel,   1, 1);
    statGrid->addWidget(m_heartbeatLabel, 2, 0);
    statGrid->addWidget(m_timeLabel,  2, 1);

    /* 电脑当前锁定状态：由固件在输入报告第 3 字节回传，
     * 按电脑键盘还是板载 X/Y 都会同步更新 */
    m_capsLabel = new QLabel(keyBox);
    m_numLabel  = new QLabel(keyBox);
    for (QLabel *lab : { m_capsLabel, m_numLabel }) {
        lab->setAlignment(Qt::AlignCenter);
        lab->setMinimumHeight(22);
    }
    statGrid->addWidget(m_capsLabel, 3, 0);
    statGrid->addWidget(m_numLabel,  3, 1);
    updateLockBadges();
    keyLay->addLayout(statGrid);

    m_eventLabel = new QLabel(QStringLiteral("最近事件：无"), keyBox);
    m_eventLabel->setWordWrap(true);
    m_eventLabel->setMinimumHeight(48);
    m_eventLabel->setStyleSheet(QStringLiteral("background:#F6F7F9;border:1px solid #E3E6EA;border-radius:4px;padding:5px;color:#444;"));
    keyLay->addWidget(m_eventLabel, 1);

    auto *clearCnt = new QPushButton(QStringLiteral("清零计数"), keyBox);
    connect(clearCnt, &QPushButton::clicked, this, &MainWindow::onClearCounters);
    keyLay->addWidget(clearCnt);
    /* 按键区给个上限：窗口拉宽时把多余空间让给 RGB 面板，而不是把圆灯和计数拉开 */
    keyBox->setMaximumWidth(540);
    body->addWidget(keyBox, 1);

    /* 右：RGB 控制 */
    m_rgbBox = new QGroupBox(QStringLiteral("RGB 灯控制（自定义 HID 输出报告）"), central);
    auto *rgbLay = new QVBoxLayout(m_rgbBox);
    auto *sliderForm = new QGridLayout;
    sliderForm->setSpacing(8);
    const char *channelNames[3] = { "R (红)", "G (绿)", "B (蓝)" };
    const char *channelColor[3] = {
        "#E43B44", "#4CAF50", "#3F7CE0"
    };
    for (int i = 0; i < 3; ++i) {
        auto *lab = new QLabel(QString::fromUtf8(channelNames[i]), m_rgbBox);
        lab->setStyleSheet(QString("color:%1;font-weight:bold;").arg(channelColor[i]));
        auto *sl = new QSlider(Qt::Horizontal, m_rgbBox);
        sl->setRange(0, 100);
        sl->setValue(0);
        auto *sp = new QSpinBox(m_rgbBox);
        sp->setRange(0, 100);
        sp->setValue(0);
        sp->setSuffix(QStringLiteral(" %"));
        sliderForm->addWidget(lab, i, 0);
        sliderForm->addWidget(sl, i, 1);
        sliderForm->addWidget(sp, i, 2);
        m_sliders.append(sl);
        m_spinBoxes.append(sp);
        connect(sl, &QSlider::valueChanged, sp, &QSpinBox::setValue);
        connect(sp, QOverload<int>::of(&QSpinBox::valueChanged), sl, &QSlider::setValue);
        connect(sl, &QSlider::valueChanged, this, &MainWindow::onSliderChanged);
    }
    rgbLay->addLayout(sliderForm);

    m_colorPreview = new ColorSwatch(m_rgbBox);
    rgbLay->addWidget(m_colorPreview);

    /* 预设色按钮 */
    auto *presetLay = new QGridLayout;
    presetLay->setSpacing(5);
    struct Preset { const char *name; int r, g, b; };
    static const Preset presets[] = {
        { "关灯",   0,   0,   0  },
        { "红",   100,   0,   0  },
        { "绿",     0, 100,   0  },
        { "蓝",     0,   0, 100  },
        { "黄",   100, 100,   0  },
        { "青",     0, 100, 100  },
        { "紫",   100,   0, 100  },
        { "白",   100, 100, 100  }
    };
    for (int i = 0; i < 8; ++i) {
        auto *b = new QPushButton(QString::fromUtf8(presets[i].name), m_rgbBox);
        b->setProperty("rgb", QList<QVariant>{presets[i].r, presets[i].g, presets[i].b});
        connect(b, &QPushButton::clicked, this, &MainWindow::onPresetClicked);
        presetLay->addWidget(b, i / 4, i % 4);
    }
    rgbLay->addLayout(presetLay);

    auto *optLay = new QHBoxLayout;
    m_liveSend = new QCheckBox(QStringLiteral("滑动即下发"), m_rgbBox);
    m_liveSend->setChecked(true);
    m_rainbow = new QCheckBox(QStringLiteral("彩虹渐变"), m_rgbBox);
    optLay->addWidget(m_liveSend);
    optLay->addWidget(m_rainbow);
    rgbLay->addLayout(optLay);
    connect(m_rainbow, &QCheckBox::toggled, this, [this](bool on) {
        if (on) m_rainbowTimer.start(); else m_rainbowTimer.stop();
        sendRainbowMode(on);
    });

    auto *sendBtn = new QPushButton(QStringLiteral("发送 RGB 到板子"), m_rgbBox);
    sendBtn->setMinimumHeight(36);
    sendBtn->setStyleSheet(QStringLiteral("background:#2D7DD2;color:white;font-weight:bold;border-radius:4px;"));
    connect(sendBtn, &QPushButton::clicked, this, &MainWindow::onSendRgb);
    rgbLay->addWidget(sendBtn);
    rgbLay->addStretch(1);
    /* 右栏保底宽度：窗口窄的时候优先压缩日志，不允许把滑块和按钮挤没 */
    m_rgbBox->setMinimumWidth(400);
    body->addWidget(m_rgbBox, 1);

    auto *workspace = new QTabWidget(central);
    auto *monitorPage = new QWidget(workspace);
    monitorPage->setLayout(body);
    workspace->addTab(makeScrollPage(monitorPage, 590), QStringLiteral("实时监测"));
    root->addWidget(workspace, 1);

    m_assessmentBar = new Dashboard::AssessmentBar(central);
    root->addWidget(m_assessmentBar);
    connect(m_assessmentBar->detailsButton(), &QPushButton::clicked,
            this, &MainWindow::onShowAssessment);
    connect(m_assessmentBar->testButton(), &QPushButton::clicked,
            this, &MainWindow::onStartRgbTest);
    connect(m_assessmentBar->exportButton(), &QPushButton::clicked,
            this, &MainWindow::onExportReport);

    /* ---------- 日志 ---------- */
    auto *logBox = new QGroupBox(QStringLiteral("通信日志"), central);
    auto *logLay = new QVBoxLayout(logBox);

    /* 状态读取和原始命令放在诊断页，保持监测页的 RGB 控件完整可见。 */
    auto *statusBtn = new QPushButton(QStringLiteral("发送 GET_STATUS (0x02)"), logBox);
    connect(statusBtn, &QPushButton::clicked, this, &MainWindow::onReadStatus);
    logLay->addWidget(statusBtn);

    auto *rawLay = new QHBoxLayout;
    m_rawEdit = new QLineEdit(logBox);
    m_rawEdit->setPlaceholderText(QStringLiteral("8 字节报告体，如 01 64 32 00；Report ID 自动匹配当前链路"));
    auto *rawBtn = new QPushButton(QStringLiteral("发送"), logBox);
    rawLay->addWidget(m_rawEdit, 1);
    rawLay->addWidget(rawBtn);
    connect(rawBtn, &QPushButton::clicked, this, &MainWindow::onSendRaw);
    connect(m_rawEdit, &QLineEdit::returnPressed, this, &MainWindow::onSendRaw);
    logLay->addLayout(rawLay);

    m_logView = new QPlainTextEdit(logBox);
    m_logView->setReadOnly(true);
    /* 行数上限 + 关掉长行折行 + 关掉撤销栈：长时间跑日志不会越用越卡 */
    m_logView->setMaximumBlockCount(1200);
    m_logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_logView->setUndoRedoEnabled(false);
    QFont mono(QStringLiteral("Consolas"), 9);
    m_logView->setFont(mono);
    logLay->addWidget(m_logView);
    auto *logBtnLay = new QHBoxLayout;
    m_pauseLog = new QCheckBox(QStringLiteral("暂停滚动"), logBox);
    /* 取消暂停时自动跳到底部，恢复"跟随最新日志" */
    connect(m_pauseLog, &QCheckBox::toggled, this, [this](bool checked) {
        if (!checked && m_logView)
            m_logView->verticalScrollBar()->setValue(m_logView->verticalScrollBar()->maximum());
    });
    m_logEveryFrame = new QCheckBox(QStringLiteral("逐帧记录"), logBox);
    m_logEveryFrame->setToolTip(QStringLiteral("默认按 1 秒 / 按键变化记录输入报告，勾选后每个报告都写一行"));
    auto *clearBtn = new QPushButton(QStringLiteral("清空日志"), logBox);
    auto *saveBtn = new QPushButton(QStringLiteral("保存日志"), logBox);
    connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearLog);
    connect(saveBtn, &QPushButton::clicked, this, &MainWindow::onSaveLog);
    logBtnLay->addWidget(m_pauseLog);
    logBtnLay->addWidget(m_logEveryFrame);
    logBtnLay->addStretch(1);
    logBtnLay->addWidget(clearBtn);
    logBtnLay->addWidget(saveBtn);
    logLay->addLayout(logBtnLay);
    logBox->setTitle(QStringLiteral("原始数据与事件"));
    workspace->addTab(makeScrollPage(logBox, 480), QStringLiteral("通信日志"));

    /* ---------- 设备配置：USB Feature Report，写入时才擦写 Flash ---------- */
    m_configBox = new QGroupBox(QStringLiteral("改键与摇杆参数（通过 USB 配置，USB / 蓝牙输出共用）"), workspace);
    auto *configLay = new QVBoxLayout(m_configBox);
    auto *keyGridConfig = new QGridLayout;
    const char *configNames[9] = { "A", "B", "X", "Y", "UP", "DN", "LF", "L3", "R3" };
    for (int i = 0; i < 9; ++i) {
        auto *combo = new QComboBox(m_configBox);
        combo->addItem(QStringLiteral("无动作"), 0x0000);
        combo->addItem(QStringLiteral("鼠标左键"), Proto::actionMouse(1));
        combo->addItem(QStringLiteral("鼠标右键"), Proto::actionMouse(2));
        combo->addItem(QStringLiteral("鼠标中键"), Proto::actionMouse(4));
        for (int key = 0; key < 26; ++key)
            combo->addItem(QStringLiteral("键盘 %1").arg(QChar('A' + key)), Proto::actionKeyboard(quint8(0x04 + key)));
        for (int key = 0; key < 10; ++key) {
            const int usage = (key == 0) ? 0x27 : (0x1D + key);
            combo->addItem(QStringLiteral("键盘 %1").arg(key), Proto::actionKeyboard(quint8(usage)));
        }
        const struct { const char *name; quint8 usage; } specials[] = {
            { "回车 Enter", 0x28 }, { "Esc", 0x29 }, { "退格 Backspace", 0x2A },
            { "Tab", 0x2B }, { "空格 Space", 0x2C }, { "Caps Lock", 0x39 },
            { "Num Lock", 0x53 }, { "F1", 0x3A }, { "F2", 0x3B }, { "F3", 0x3C },
            { "F4", 0x3D }, { "方向 →", 0x4F }, { "方向 ←", 0x50 },
            { "方向 ↓", 0x51 }, { "方向 ↑", 0x52 }
        };
        for (const auto &item : specials)
            combo->addItem(QString::fromUtf8(item.name), Proto::actionKeyboard(item.usage));
        combo->addItem(QStringLiteral("左 Ctrl"), Proto::actionModifier(0x01));
        combo->addItem(QStringLiteral("左 Shift"), Proto::actionModifier(0x02));
        combo->addItem(QStringLiteral("左 Alt"), Proto::actionModifier(0x04));
        combo->addItem(QStringLiteral("左 Win"), Proto::actionModifier(0x08));
        combo->addItem(QStringLiteral("右 Ctrl"), Proto::actionModifier(0x10));
        combo->addItem(QStringLiteral("右 Shift"), Proto::actionModifier(0x20));
        combo->addItem(QStringLiteral("右 Alt"), Proto::actionModifier(0x40));
        combo->addItem(QStringLiteral("右 Win"), Proto::actionModifier(0x80));
        m_actionCombos.append(combo);
        const int labelColumn = (i % 3) * 2;
        auto *keyLabel = new QLabel(QString::fromUtf8(configNames[i]), m_configBox);
        keyLabel->setFixedWidth(38);
        keyLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        keyGridConfig->addWidget(keyLabel, i / 3, labelColumn);
        keyGridConfig->addWidget(combo, i / 3, labelColumn + 1);
        keyGridConfig->setColumnStretch(labelColumn, 0);
        keyGridConfig->setColumnStretch(labelColumn + 1, 1);
    }
    configLay->addLayout(keyGridConfig);

    auto *joyGrid = new QGridLayout;
    const char *stickNames[2] = { "左摇杆（光标）", "右摇杆（滚轮）" };
    for (int i = 0; i < 2; ++i) {
        joyGrid->addWidget(new QLabel(QString::fromUtf8(stickNames[i]), m_configBox), i, 0);
        m_sens[i] = new QSpinBox(m_configBox);
        m_sens[i]->setRange(25, 200); m_sens[i]->setSingleStep(5); m_sens[i]->setSuffix(QStringLiteral(" %"));
        m_deadzone[i] = new QSpinBox(m_configBox);
        m_deadzone[i]->setRange(0, 24000); m_deadzone[i]->setSingleStep(500);
        joyGrid->addWidget(new QLabel(QStringLiteral("灵敏度"), m_configBox), i, 1);
        joyGrid->addWidget(m_sens[i], i, 2);
        joyGrid->addWidget(new QLabel(QStringLiteral("死区 / 阈值"), m_configBox), i, 3);
        joyGrid->addWidget(m_deadzone[i], i, 4);
    }
    configLay->addLayout(joyGrid);
    auto *configButtons = new QHBoxLayout;
    auto *readConfig = new QPushButton(QStringLiteral("读取配置"), m_configBox);
    auto *writeConfig = new QPushButton(QStringLiteral("写入并保存"), m_configBox);
    auto *defaultConfig = new QPushButton(QStringLiteral("恢复默认并保存"), m_configBox);
    writeConfig->setProperty("primary", true);
    configButtons->addWidget(readConfig); configButtons->addWidget(writeConfig);
    configButtons->addWidget(defaultConfig); configButtons->addStretch(1);
    configLay->addLayout(configButtons);
    m_configStatus = new QLabel(QStringLiteral("USB 连接后可读取；只有点击保存才写 Flash。"), m_configBox);
    m_configStatus->setWordWrap(true);
    configLay->addWidget(m_configStatus);
    workspace->addTab(makeScrollPage(m_configBox, 420), QStringLiteral("设备配置"));
    connect(readConfig, &QPushButton::clicked, this, &MainWindow::onConfigRead);
    connect(writeConfig, &QPushButton::clicked, this, &MainWindow::onConfigWrite);
    connect(defaultConfig, &QPushButton::clicked, this, &MainWindow::onConfigDefault);

    m_performance = new PerformanceMonitor(workspace);
    m_performance->setSequenceSupport(true);
    workspace->addTab(makeScrollPage(m_performance, 700), QStringLiteral("性能测试"));
    connect(m_performance, &PerformanceMonitor::resetRequested,
            this, &MainWindow::onClearCounters);
    connect(m_performance, &PerformanceMonitor::exportRequested,
            this, &MainWindow::onExportReport);

    /* ---------- status bar ---------- */
    m_modeLabel = new QLabel(this);
    statusBar()->addWidget(m_modeLabel);         /* 工作模式放最左边，一眼可见 */
    m_statusLabel = new QLabel(this);
    statusBar()->addWidget(m_statusLabel);

    /* 鼠标点击按钮或复选框后不再保留键盘焦点，避免板载 UP（空格）或电脑空格
     * 再次激活上一次点击的控件；鼠标点击功能保持不变。 */
    const auto buttons = findChildren<QAbstractButton *>();
    for (QAbstractButton *button : buttons) {
        button->setFocusPolicy(Qt::NoFocus);
    }

    setStyleSheet(Dashboard::styleSheet(QStringLiteral("#1677B8"),
                                        QStringLiteral("#0E5A8A")));
    setConnected(false);
}

/* ---------------------------------------------------------------------------
 * Device handling
 * ------------------------------------------------------------------------- */
void MainWindow::populateDeviceCombo()
{
    m_deviceCombo->clear();
    for (const HidDeviceInfo &d : m_devices) {
        m_deviceCombo->addItem(d.label(), d.path);
        const QString tip = QStringLiteral("链路：%1\n产品名：%2\n序列号：%3\n报告长度：IN %4B / OUT %5B\n自定义 Report ID：%6\n路径：%7")
                                .arg(d.transportName())
                                .arg(d.product.isEmpty() ? QStringLiteral("(无)") : d.product)
                                .arg(d.serial.isEmpty() ? QStringLiteral("(无)") : d.serial)
                                .arg(d.inputReportLen)
                                .arg(d.outputReportLen)
                                .arg(d.reportId())
                                .arg(d.path);
        m_deviceCombo->setItemData(m_deviceCombo->count() - 1, tip, Qt::ToolTipRole);
    }
    if (m_devices.isEmpty())
        m_deviceCombo->addItem(QStringLiteral("未找到设备（请连接 USB 或配对 MP KEYMOUSE）"));
}

void MainWindow::onRefreshDevices()
{
    const QString old = m_deviceCombo->currentData().toString();
    m_devices = HidDevice::enumerate();
    populateDeviceCombo();
    if (!old.isEmpty()) {
        int idx = m_deviceCombo->findData(old);
        if (idx >= 0) m_deviceCombo->setCurrentIndex(idx);
    }
    appendLog(QStringLiteral("SYS"), QStringLiteral("枚举到 %1 个自定义 HID 接口（USB 排前）").arg(m_devices.size()));
}

void MainWindow::setConnected(bool on)
{
    m_connected = on;
    m_performance->setConnectionState(on);
    m_connectBtn->setText(on ? QStringLiteral("断开连接") : QStringLiteral("连接设备"));
    m_deviceCombo->setEnabled(!on);
    m_refreshBtn->setEnabled(!on);
    m_rgbBox->setEnabled(on);
    m_configBox->setEnabled(on && !m_hid.info().isBluetooth());
    if (!on) {
        m_configWaitingSave = false;
        m_configPollTimer.stop();
    }
    if (on) {
        if (!m_hid.info().isBluetooth()) QTimer::singleShot(200, this, &MainWindow::onConfigRead);
        m_connLabel->setText(m_hid.info().transportName() + QStringLiteral(" 已连接"));
        m_connLabel->setStyleSheet(QStringLiteral("color:#7EE2B8;font-weight:bold;"));
        m_connTime.restart();
        m_frames = 0;
        m_lostFrames = 0;
        m_lastSeq = -1;
        m_lastBitmap = 0;
        m_lastStickBitmap = 0;
        m_testedKeys = 0;
        m_testedSticks = 0;
        m_seenLedState = 0;
        m_rgbBaseline = m_hid.outOkCount();
        for (KeyIndicator *k : m_keys) {
            k->setDown(false);
            k->resetCount();
        }
        if (m_rainbow->isChecked()) {
            m_rainbowTimer.start();
            sendRainbowMode(true);
        }
    } else {
        m_connLabel->setText(QStringLiteral("未连接"));
        m_connLabel->setStyleSheet(QStringLiteral("color:#C4D5E2;font-weight:bold;"));
        m_lastSeq = -1;
        m_ledState = 0;
        m_rainbowTimer.stop();
        m_rgbTestTimer.stop();
        m_assessmentBar->testButton()->setText(QStringLiteral("RGB 通道自检"));
        for (KeyIndicator *k : m_keys) k->setDown(false);
    }
    updateLockBadges();
    m_statusLabel->setText(QStringLiteral("链路：%1  |  VID:%2 PID:%3  |  自定义 Report ID:%4  |  9B 输入 / 8B 输出  |  USB 枚举后输入优先走 USB")
        .arg(on ? m_hid.info().transportName() : QStringLiteral("未连接"))
        .arg(on ? m_hid.info().vid : 0, 4, 16, QChar('0'))
        .arg(on ? m_hid.info().pid : 0, 4, 16, QChar('0'))
        .arg(on ? m_hid.info().reportId() : 0));
    updateStats();
    updateAssessment();
}

void MainWindow::onConnectClicked()
{
    if (m_connected) {
        m_connectWanted = false;
        m_hid.close();
        setConnected(false);
        appendLog(QStringLiteral("SYS"), QStringLiteral("已断开"));
        return;
    }
    const int idx = m_deviceCombo->currentIndex();
    if (idx < 0 || idx >= m_devices.size()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请连接 USB，或先在 Windows 蓝牙设置中配对 MP KEYMOUSE。"));
        return;
    }
    HidDeviceInfo info = m_devices.at(idx);
    if (!m_hid.open(info)) {
        QMessageBox::warning(this, QStringLiteral("连接失败"), QStringLiteral("打不开 HID 设备。请确认固件、蓝牙配对和其他调试工具状态。"));
        return;
    }
    m_reconnectPath = info.path;
    m_connectWanted = true;
    setConnected(true);
    appendLog(QStringLiteral("SYS"), QStringLiteral("当前使用 %1 链路，自定义 Report ID=%2")
              .arg(info.transportName()).arg(info.reportId()));
}

void MainWindow::onDeviceLost(const QString &reason)
{
    m_hid.close();
    setConnected(false);
    appendLog(QStringLiteral("ERR"), reason, QColor(0xB0, 0x00, 0x20));
}

void MainWindow::onAutoReconnectTick()
{
    if (!m_connectWanted || m_connected || !m_autoReconnect->isChecked()) return;
    QList<HidDeviceInfo> now = HidDevice::enumerate();
    m_devices = now;
    populateDeviceCombo();
    for (const HidDeviceInfo &info : now) {
        if (info.path != m_reconnectPath) continue;
        if (m_hid.open(info)) {
            setConnected(true);
            appendLog(QStringLiteral("SYS"), info.transportName() + QStringLiteral(" 自动重连成功"), QColor(0x1E, 0x8E, 0x3E));
        }
        return;
    }
    /* 原链路掉线时不偷偷跳到另一种传输，避免误操作和统计口径混淆。 */
    statusBar()->showMessage(QStringLiteral("等待原设备重新上线；不会自动切换 USB／蓝牙"), 1800);
}

void MainWindow::onLog(const QString &text)
{
    appendLog(QStringLiteral("HID"), text);
}

/* ---------------------------------------------------------------------------
 * Input report: parse key bitmap (assessment item 3.1)
 * ------------------------------------------------------------------------- */
void MainWindow::onReport(const QByteArray &raw)
{
    const qint64 perfNow = QDateTime::currentMSecsSinceEpoch();
    QByteArray body = Proto::stripReportId(raw, m_hid.info().reportId());
    if (body.size() < 2) {
        m_performance->recordInput(perfNow, false);
        return;
    }
    const quint8 bm = quint8(body.at(0));
    const quint8 seq = quint8(body.at(1));
    const quint8 led = (body.size() > 2) ? quint8(body.at(2)) : quint8(0);
    const quint8 st  = (body.size() > 3) ? quint8(body.at(3)) : quint8(0);
    if (body.size() > 6) {
        m_diagReason = quint8(body.at(4));
        m_diagTries  = quint8(body.at(5));
        m_diagState  = quint8(body.at(6));
    }
    if (body.size() > 7)
        m_diagChan = quint8(body.at(7));
    const quint8 joy = (body.size() > 8) ? quint8(body.at(8)) : quint8(0);
    const quint64 now = quint64(m_connTime.elapsed());
    const bool changed = (bm != m_lastBitmap) || (joy != m_lastStickBitmap);

    if (st != m_lastStatus) {
        m_lastStatus = st;
        appendLog(QStringLiteral("SYS"),
                  QStringLiteral("工作模式：%1").arg(Proto::statusText(st)),
                  QColor(0x8A, 0x2B, 0xE2));
    }

    ++m_frames;
    m_lastFrameMs = now;
    m_ledState = led;
    m_testedKeys = quint8(m_testedKeys | bm);
    m_testedSticks = quint8(m_testedSticks | joy);
    m_seenLedState = quint8(m_seenLedState | led);
    m_uiDirty = true;

    quint64 lost = 0;
    if (m_lastSeq >= 0 && seq != quint8((m_lastSeq + 1) & 0xFF)) {
        lost = quint8(seq - quint8(m_lastSeq) - 1);
        m_lostFrames += lost;
    }
    m_lastSeq = seq;
    m_performance->recordInput(perfNow, true, lost);

    /* 逐个按键比较，只有边沿才记事件并计数，心跳帧不会重复计数 */
    for (int i = 0; i < 10; ++i) {
        const bool nowDown = (i < 8) ? ((bm & (1u << i)) != 0)
                                     : ((joy & (1u << (i - 8))) != 0);
        const bool before = (i < 8) ? ((m_lastBitmap & (1u << i)) != 0)
                                    : ((m_lastStickBitmap & (1u << (i - 8))) != 0);
        if (nowDown == before) continue;
        KeyIndicator *ind = m_keys.at(i);
        ind->setDown(nowDown);
        if (nowDown) {
            ind->setPressCount(ind->pressCount() + 1);
            m_events.prepend(QStringLiteral("%1 按下").arg(QString::fromUtf8(Proto::kKeys[i].name)));
        } else {
            m_events.prepend(QStringLiteral("%1 松开").arg(QString::fromUtf8(Proto::kKeys[i].name)));
        }
    }
    m_lastBitmap = bm;
    m_lastStickBitmap = joy;
    if (m_events.size() > 6) m_events = m_events.mid(0, 6);

    /* 面板文本交给 20Hz 的刷新器统一更新，报告再快也不会疯狂 relayout */
    m_uiDirty = true;

    /* 日志按“按键变化 / 锁定变化 / 逐帧 / 至少 900ms 一条”抽样，避免日志把界面拖慢 */
    const bool logIt = (m_logEveryFrame && m_logEveryFrame->isChecked())
                       || changed || (now - quint64(m_lastFrameLogMs) >= 900);
    if (logIt) {
        m_lastFrameLogMs = qint64(now);
        appendLog(QStringLiteral("IN "), Proto::hexDump(raw) + QStringLiteral("  ->  ")
                  + QString::fromUtf8(Proto::bitmapToText(bm, joy).toUtf8())
                  + QStringLiteral("   [CAPS %1 NUM %2]")
                        .arg((led & 0x02U) ? 1 : 0).arg((led & 0x01U) ? 1 : 0));
    }
}

void MainWindow::refreshUi()
{
    if (!m_uiDirty) return;
    m_uiDirty = false;

    auto setIfChanged = [](QLabel *lab, const QString &s) {
        if (lab && lab->text() != s) lab->setText(s);
    };

    const quint8 bm = m_lastBitmap;
    const quint8 joy = m_lastStickBitmap;
    setIfChanged(m_bmLabel, QStringLiteral("位图 0x%1 / 摇杆 0x%2  %3")
                     .arg(bm, 2, 16, QChar('0'))
                     .arg(joy, 2, 16, QChar('0')).toUpper()
                     .arg((bm || joy) ? QString::fromUtf8(Proto::bitmapToText(bm, joy).toUtf8())
                                      : QStringLiteral("（无按键）")));
    setIfChanged(m_modeLabel, m_connected
                     ? QStringLiteral("模式：%1  |  BT状态=%2 回连=%3 断开码=0x%4  通道=0x%5")
                           .arg(Proto::statusText(m_lastStatus))
                           .arg(m_diagState)
                           .arg(m_diagTries)
                           .arg(QString::number(m_diagReason, 16).toUpper().rightJustified(2, QLatin1Char('0')))
                           .arg(QString::number(m_diagChan, 16).toUpper().rightJustified(2, QLatin1Char('0')))
                     : QStringLiteral("模式：—"));

    /* 连接徽章也带上"键鼠输出走哪条通道"，避免只看"USB 已连接"误以为还在 USB 模式 */
    if (m_connected) {
        const bool outBt = (m_lastStatus & Proto::kStForceBt) != 0u;
        setIfChanged(m_connLabel,
                     QStringLiteral("%1 已连接 · 键鼠输出：%2")
                         .arg(m_hid.info().transportName())
                         .arg(outBt ? QString::fromUtf8("蓝牙") : QString::fromUtf8("USB")));
    }
    setIfChanged(m_seqLabel, QStringLiteral("序号 %1").arg(m_lastSeq < 0 ? 0 : m_lastSeq));
    setIfChanged(m_frameLabel, QStringLiteral("报告 %1 帧").arg(m_frames));
    setIfChanged(m_lostLabel, QStringLiteral("丢帧 %1").arg(m_lostFrames));
    setIfChanged(m_eventLabel, QStringLiteral("最近事件：%1")
                     .arg(m_events.isEmpty() ? QStringLiteral("无")
                                             : m_events.join(QStringLiteral("  /  "))));
    updateLockBadges();
}

/* Caps / Num 锁定状态徽标。文本没变就不动样式表，避免每帧重新 polish */
void MainWindow::updateLockBadges()
{
    const bool caps = (m_ledState & 0x02U) != 0U;
    const bool num  = (m_ledState & 0x01U) != 0U;

    const QString on  = QStringLiteral("background:#2D7DD2;color:white;border-radius:3px;font-weight:bold;padding:1px;");
    const QString off = QStringLiteral("background:#EDEFF3;color:#9AA1AB;border-radius:3px;padding:1px;");

    if (m_capsLabel) {
        const QString t = caps ? QStringLiteral("CAPS 锁定") : QStringLiteral("CAPS 未锁");
        if (m_capsLabel->text() != t) {
            m_capsLabel->setText(t);
            m_capsLabel->setStyleSheet(caps ? on : off);
        }
    }
    if (m_numLabel) {
        const QString t = num ? QStringLiteral("NUM 锁定") : QStringLiteral("NUM 未锁");
        if (m_numLabel->text() != t) {
            m_numLabel->setText(t);
            m_numLabel->setStyleSheet(num ? on : off);
        }
    }
}

void MainWindow::onUiTick()
{
    refreshUi();
}

void MainWindow::onSendTick()
{
    if (!m_rgbDirty) return;
    m_rgbDirty = false;
    sendRgbNow(m_curRgb[0], m_curRgb[1], m_curRgb[2]);
}

void MainWindow::onFlushLog()
{
    if (!m_logView || m_logBuf.isEmpty()) return;
    const QString chunk = m_logBuf.join(QChar('\n'));
    m_logBuf.clear();

    QScrollBar *sb = m_logView->verticalScrollBar();
    const int keep = sb->value();

    /* 整批一次写进去，比逐行 appendPlainText 少几十倍布局开销 */
    m_logView->appendPlainText(chunk);

    /* 暂停滚动：无条件把位置恢复回去。
     * appendPlainText() 每次都会把视图拉到底部，之前只在"不在底部"时
     * 才恢复位置，所以用户停在底部时暂停就失效了（日志照样一直往下滚）。 */
    if (m_pauseLog && m_pauseLog->isChecked())
        sb->setValue(keep);
}

void MainWindow::onHeartbeatTick()
{
    const quint64 now = quint64(m_connTime.elapsed());
    if (m_connected) {
        const bool alive = (now - m_lastFrameMs) < 2500;
        const QString s = alive ? QStringLiteral("心跳 正常") : QStringLiteral("心跳 超时");
        if (m_heartbeatLabel->text() != s) {
            m_heartbeatLabel->setText(s);
            m_heartbeatLabel->setStyleSheet(alive ? QStringLiteral("color:#1E8E3E;")
                                                  : QStringLiteral("color:#B00020;"));
        }
        const QString t = QStringLiteral("连接 %1 秒").arg(now / 1000);
        if (m_timeLabel->text() != t) m_timeLabel->setText(t);
    } else if (m_heartbeatLabel->text() != QStringLiteral("心跳 超时")) {
        m_heartbeatLabel->setText(QStringLiteral("心跳 超时"));
    }
    updateStats();
    updateAssessment();
}

void MainWindow::appendLog(const QString &tag, const QString &text, const QColor &color)
{
    if (!m_logView) return;
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
    m_logBuf.append(QStringLiteral("[%1] %2  %3").arg(stamp, tag, text));
    if (m_logBuf.size() > 500) m_logBuf.removeFirst();
    if (!m_logTimer.isActive()) m_logTimer.start();
    (void)color;
}

void MainWindow::onClearLog() { m_logBuf.clear(); m_logView->clear(); }

void MainWindow::onSaveLog()
{
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存日志"),
        QStringLiteral("hid_log.txt"), QStringLiteral("Text (*.txt)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream out(&f);
        out << m_logView->toPlainText();
    } else QMessageBox::warning(this, QStringLiteral("保存失败"), path);
}

void MainWindow::onClearCounters()
{
    m_frames = 0;
    m_lostFrames = 0;
    m_lastSeq = -1;
    m_testedKeys = 0;
    m_testedSticks = 0;
    m_seenLedState = 0;
    m_rgbBaseline = m_hid.outOkCount();
    m_performance->resetSession(m_hid.outOkCount());
    for (KeyIndicator *k : m_keys) k->resetCount();
    m_events.clear();
    m_uiDirty = true;
    updateStats();
    updateAssessment();
    refreshUi();
}

void MainWindow::updateStats()
{
    m_performance->tick(QDateTime::currentMSecsSinceEpoch(), m_hid.outOkCount());
    m_frameLabel->setText(QStringLiteral("报告 %1 帧").arg(m_frames));
    m_lostLabel->setText(QStringLiteral("丢帧 %1").arg(m_lostFrames));
    int tested = 0;
    int testedSticks = 0;
    for (int i = 0; i < 8; ++i) tested += ((m_testedKeys & (1u << i)) != 0) ? 1 : 0;
    for (int i = 0; i < 2; ++i) testedSticks += ((m_testedSticks & (1u << i)) != 0) ? 1 : 0;
    m_framesCard->setValue(QString::number(m_frames));
    m_keysCard->setValue(QStringLiteral("%1/8 · 摇杆 %2/2").arg(tested).arg(testedSticks));
    m_lossCard->setValue(QString::number(m_lostFrames));
    m_rgbCard->setValue(QString::number(m_hid.outOkCount()));
}

/* ---------------------------------------------------------------------------
 * Output report: RGB control (assessment item 3.2)
 * ------------------------------------------------------------------------- */
void MainWindow::applyColor(int r, int g, int b, bool send)
{
    const int values[3] = { r, g, b };
    for (int i = 0; i < 3; ++i) {
        m_sliders[i]->blockSignals(true);
        m_spinBoxes[i]->blockSignals(true);
        m_sliders[i]->setValue(values[i]);
        m_spinBoxes[i]->setValue(values[i]);
        m_sliders[i]->blockSignals(false);
        m_spinBoxes[i]->blockSignals(false);
    }
    m_colorPreview->setColor(r, g, b);
    m_curRgb[0] = r;
    m_curRgb[1] = g;
    m_curRgb[2] = b;
    if (send) queueRgb(r, g, b);
}

/* 拖动滑块/彩虹动画会产生几十 Hz 的请求，这里只标记“最新值”，
 * 由 40ms 的定时器统一发一次，既不丢最后的颜色也不会把端点塞满。 */
void MainWindow::queueRgb(int r, int g, int b)
{
    m_curRgb[0] = r;
    m_curRgb[1] = g;
    m_curRgb[2] = b;
    m_rgbDirty = true;
    if (!m_sendTimer.isActive()) m_sendTimer.start();
}

void MainWindow::sendRgbNow(int r, int g, int b)
{
    if (!m_connected) return;
    QByteArray frame = Proto::makeRgb(r, g, b, m_hid.info().reportId());
    if (!m_hid.sendOutputReport(frame)) return;

    /* 发送日志 250ms 抽样一条，连续拖动时日志不会刷爆 */
    const qint64 now = m_connTime.elapsed();
    if (now - m_lastOutLogMs >= 250) {
        m_lastOutLogMs = now;
        appendLog(QStringLiteral("OUT"), Proto::hexDump(frame)
                  + QStringLiteral("  发送 SET_RGB  R=%1 G=%2 B=%3").arg(r).arg(g).arg(b),
                  QColor(0x2D, 0x7D, 0xD2));
    }
}

void MainWindow::sendRainbowMode(bool on)
{
    if (!m_connected) return;
    const quint8 cmd = on ? Proto::kCmdRainbowStart : Proto::kCmdRainbowStop;
    QByteArray frame = Proto::makeFrame(m_hid.info().reportId(), cmd);
    if (m_hid.sendOutputReport(frame)) {
        appendLog(QStringLiteral("OUT"), Proto::hexDump(frame)
                  + (on ? QStringLiteral("  启动板端彩虹渐变")
                        : QStringLiteral("  停止板端彩虹渐变")));
    }
}

void MainWindow::onSendRgb()
{
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("未连接设备，无法下发"));
        return;
    }
    if (m_rainbow->isChecked()) m_rainbow->setChecked(false);
    sendRgbNow(m_sliders.at(0)->value(), m_sliders.at(1)->value(), m_sliders.at(2)->value());
}

void MainWindow::onSliderChanged(int)
{
    if (m_rainbow->isChecked()) m_rainbow->setChecked(false);
    const int r = m_sliders.at(0)->value();
    const int g = m_sliders.at(1)->value();
    const int b = m_sliders.at(2)->value();
    applyColor(r, g, b, m_liveSend->isChecked());
}

void MainWindow::onPresetClicked()
{
    QObject *s = sender();
    QList<QVariant> v = s->property("rgb").toList();
    if (v.size() != 3) return;
    for (int i = 0; i < 3; ++i) m_sliders.at(i)->setValue(v.at(i).toInt());
    applyColor(v.at(0).toInt(), v.at(1).toInt(), v.at(2).toInt(), true);
}

void MainWindow::onRainbowTick()
{
    if (!m_connected) return;
    m_rainbowHue = (m_rainbowHue + 4) % 360;
    QColor c = QColor::fromHsv(m_rainbowHue, 255, 255);
    const int r = qRound(c.redF() * 100);
    const int g = qRound(c.greenF() * 100);
    const int b = qRound(c.blueF() * 100);
    /* 只更新界面预览；板端独立渐变，不发送连续 USB 灯控报告。 */
    applyColor(r, g, b, false);
}

void MainWindow::onReadStatus()
{
    if (!m_connected) return;
    QByteArray frame = Proto::makeFrame(m_hid.info().reportId(), Proto::kCmdGetStatus);
    if (m_hid.sendOutputReport(frame))
        appendLog(QStringLiteral("OUT"), Proto::hexDump(frame) + QStringLiteral("  发送 GET_STATUS (0x02)"));
}

void MainWindow::onSendRaw()
{
    if (!m_connected) return;
    const QStringList parts = m_rawEdit->text().trimmed().split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return;
    if (parts.size() > Proto::kXferSize) {
        QMessageBox::warning(this, QStringLiteral("HEX 格式错误"), QStringLiteral("最多输入 8 字节报告体，或 9 字节含 Report ID 的帧。"));
        return;
    }
    QByteArray frame(Proto::kXferSize, char(0));
    frame[0] = char(m_hid.info().reportId());
    const int first = (parts.size() == Proto::kXferSize) ? 1 : 0;
    for (int i = first; i < parts.size(); ++i) {
        bool ok = false;
        const unsigned v = parts.at(i).toUInt(&ok, 16);
        if (!ok || v > 255) {
            QMessageBox::warning(this, QStringLiteral("HEX 格式错误"), QStringLiteral("请输入空格分隔的两位 HEX 字节。"));
            return;
        }
        frame[1 + i - first] = char(v);
    }
    if (m_hid.sendOutputReport(frame))
        appendLog(QStringLiteral("RAW"), Proto::hexDump(frame) + QStringLiteral("  发送原始帧（ID 已按当前链路设置）"));
}

/* ---------------------------------------------------------------------------
 * 考核助手：只记录真实 HID 输入与成功写入结果，不伪造灯光或键鼠动作。
 * ------------------------------------------------------------------------- */
void MainWindow::updateAssessment()
{
    if (!m_assessmentBar) return;
    int done = 0;
    if (m_connected) ++done;
    if (m_testedKeys == 0xFFu) ++done;
    if ((m_seenLedState & 0x02u) != 0) ++done;
    if ((m_seenLedState & 0x01u) != 0) ++done;
    if (m_hid.outOkCount() > m_rgbBaseline) ++done;
    m_assessmentBar->setState(done, 5, m_connected ? QStringLiteral("实时采集中")
                                                   : QStringLiteral("等待连接"));
}

QString MainWindow::assessmentReport() const
{
    const auto mark = [](bool ok) { return ok ? QStringLiteral("[已验证]") : QStringLiteral("[待测试]"); };
    int tested = 0;
    int testedSticks = 0;
    QStringList keyNames;
    QStringList stickNames;
    for (int i = 0; i < 2; ++i) {
        if ((m_testedSticks & (1u << i)) == 0) continue;
        ++testedSticks;
        stickNames << QString::fromUtf8(Proto::kKeys[8 + i].name);
    }
    for (int i = 0; i < 8; ++i) {
        if ((m_testedKeys & (1u << i)) == 0) continue;
        ++tested;
        keyNames << QString::fromUtf8(Proto::kKeys[i].name);
    }
    QString text;
    QTextStream out(&text);
    out << "USB / 蓝牙键鼠上位机验证报告\n"
        << "生成时间：" << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n"
        << "说明：以下结果来自真实 HID 数据，不等同于考核官最终评分。\n\n"
        << "链路：" << (m_connected ? m_hid.info().transportName() : QStringLiteral("未连接")) << "\n"
        << "设备：" << (m_hid.info().product.isEmpty() ? QStringLiteral("双模复合键鼠") : m_hid.info().product) << "\n"
        << QStringLiteral("VID:0x%1  PID:0x%2  Usage Page:0xFF00  Report ID:%3\n")
               .arg(m_hid.info().vid, 4, 16, QChar('0')).arg(m_hid.info().pid, 4, 16, QChar('0'))
               .arg(m_hid.info().reportId()).toUpper()
        << "设备路径：" << (m_hid.info().path.isEmpty() ? QStringLiteral("未连接") : m_hid.info().path) << "\n\n"
        << mark(m_connected) << " 自定义 HID 接口已连接\n"
        << mark(tested == 8) << " 8 个板载按键覆盖：" << tested << "/8（" << (keyNames.isEmpty() ? QStringLiteral("无") : keyNames.join("、")) << "）\n"
        << mark(testedSticks == 2) << " 摇杆按键覆盖：" << testedSticks << "/2（" << (stickNames.isEmpty() ? QStringLiteral("无") : stickNames.join("、")) << "；L3=左 Shift，R3=左 Ctrl）\n"
        << mark((m_seenLedState & 0x02) != 0) << " Caps Lock 开启状态已回传\n"
        << mark((m_seenLedState & 0x01) != 0) << " Num Lock 开启状态已回传\n"
        << mark(m_hid.outOkCount() > m_rgbBaseline) << " RGB 输出报告写入成功\n\n"
        << "输入报告：" << m_frames << " 帧\n"
        << "序号丢失估算：" << m_lostFrames << " 帧\n"
        << "RGB 成功写入：" << m_hid.outOkCount() << " 帧\n"
        << "当前按键位图：0x" << QString::number(m_lastBitmap, 16).toUpper().rightJustified(2, '0') << "\n"
        << "当前摇杆键位图：0x" << QString::number(m_lastStickBitmap, 16).toUpper().rightJustified(2, '0') << "\n"
        << "当前锁定状态：Caps=" << ((m_ledState & 0x02) ? "ON" : "OFF")
        << "  Num=" << ((m_ledState & 0x01) ? "ON" : "OFF") << "\n\n"
        << "仍需现场验证：Windows 中的键盘／鼠标枚举，光标与滚轮移动，左右键点击，板载 Num／Caps 指示灯和 RGB 实际颜色。\n"
        << "提示：RG 仅用于自定义 HID 上报；其他按键的电脑功能请同步观察。\n";
    return text;
}

void MainWindow::onShowAssessment()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("考核验证结果"));
    box.setIcon(QMessageBox::Information);
    box.setText(assessmentReport());
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    box.exec();
}

void MainWindow::onExportReport()
{
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出性能与验收报告"),
        QStringLiteral("usb_keymouse_performance_%1.html")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")),
        QStringLiteral("HTML 报告 (*.html);;文本报告 (*.txt)"), &selectedFilter);
    if (path.isEmpty()) return;
    const bool html = !path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)
                      && !selectedFilter.startsWith(QStringLiteral("文本"));
    if (!path.endsWith(QStringLiteral(".html"), Qt::CaseInsensitive)
        && !path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive))
        path += html ? QStringLiteral(".html") : QStringLiteral(".txt");

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    const QString functional = assessmentReport();
    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    if (html) {
        out << m_performance->reportHtml(
            QStringLiteral("USB / 蓝牙双模键鼠"),
            m_hid.info().product.isEmpty() ? QStringLiteral("双模复合键鼠") : m_hid.info().product,
            m_connected ? m_hid.info().transportName() : QStringLiteral("未连接"), functional);
    } else {
        out << functional << m_performance->reportText();
    }
    if (!file.flush()) {
        QMessageBox::warning(this, QStringLiteral("导出失败"), file.errorString());
        return;
    }
    file.close();
    appendLog(QStringLiteral("SYS"), QStringLiteral("性能与验收报告已导出：%1").arg(path));
    m_statusLabel->setText(QStringLiteral("报告已导出：%1").arg(path));
}

void MainWindow::onStartRgbTest()
{
    if (m_rgbTestTimer.isActive()) {
        m_rgbTestTimer.stop();
        m_assessmentBar->testButton()->setText(QStringLiteral("RGB 通道自检"));
        appendLog(QStringLiteral("SYS"), QStringLiteral("RGB 通道自检已停止"));
        return;
    }
    if (!m_connected) {
        QMessageBox::information(this, QStringLiteral("RGB 通道自检"),
                                 QStringLiteral("请先连接自定义 HID 接口，再执行红、绿、蓝、白、熄灭自检。"));
        return;
    }
    m_rgbTestStep = 0;
    m_assessmentBar->testButton()->setText(QStringLiteral("停止自检"));
    appendLog(QStringLiteral("SYS"), QStringLiteral("开始 RGB 通道自检：红 → 绿 → 蓝 → 白 → 熄灭"));
    onRgbTestTick();
    m_rgbTestTimer.start();
}

void MainWindow::onRgbTestTick()
{
    static const int colors[5][3] = {
        { 100, 0, 0 }, { 0, 100, 0 }, { 0, 0, 100 },
        { 100, 100, 100 }, { 0, 0, 0 }
    };
    if (!m_connected || m_rgbTestStep >= 5) {
        m_rgbTestTimer.stop();
        m_assessmentBar->testButton()->setText(QStringLiteral("RGB 通道自检"));
        if (m_rgbTestStep >= 5) appendLog(QStringLiteral("SYS"), QStringLiteral("RGB 通道自检完成，请确认板载灯光"));
        return;
    }
    applyColor(colors[m_rgbTestStep][0], colors[m_rgbTestStep][1], colors[m_rgbTestStep][2], true);
    ++m_rgbTestStep;
}

void MainWindow::onConfigRead()
{
    if (!m_connected || m_hid.info().isBluetooth()) return;
    m_configWaitingSave = false;
    m_configPollTimer.stop();
    if (!m_hid.requestFeatureReport()) {
        m_configStatus->setText(QStringLiteral("读取请求入队失败，请重新连接 USB。"));
        return;
    }
    m_configStatus->setText(QStringLiteral("正在读取设备配置…"));
}

void MainWindow::onConfigWrite()
{
    if (!m_connected || m_hid.info().isBluetooth()) return;
    QByteArray cfg(Proto::kConfigSize, char(0));
    cfg[0] = char(Proto::kConfigMagic);
    cfg[1] = char(Proto::kConfigVersion);
    cfg[2] = char(Proto::kConfigCmdSave);
    for (int i = 0; i < 9; ++i)
        Proto::put16(cfg, 4 + i * 2, quint16(m_actionCombos.at(i)->currentData().toUInt()));
    for (int i = 0; i < 2; ++i) cfg[22 + i] = char(m_sens[i]->value());
    Proto::put16(cfg, 24, quint16(m_deadzone[0]->value()));
    Proto::put16(cfg, 26, quint16(m_deadzone[1]->value()));
    Proto::sealConfig(cfg);
    if (!m_hid.sendFeatureReport(QByteArray(1, char(0)) + cfg)) {
        m_configStatus->setText(QStringLiteral("写入请求入队失败，请重新连接 USB。"));
        return;
    }
    m_configStatus->setText(QStringLiteral("正在写入配置，随后确认 Flash 保存状态…"));
}

void MainWindow::onConfigDefault()
{
    if (!m_connected || m_hid.info().isBluetooth()) return;
    QByteArray cfg = Proto::defaultConfig();
    cfg[2] = char(Proto::kConfigCmdDefault);
    Proto::sealConfig(cfg);
    if (m_hid.sendFeatureReport(QByteArray(1, char(0)) + cfg))
        m_configStatus->setText(QStringLiteral("正在恢复默认键位与摇杆参数并保存…"));
}

void MainWindow::onConfigPoll()
{
    if (!m_configWaitingSave || !m_connected) return;
    if (!m_hid.requestFeatureReport()) {
        m_configWaitingSave = false;
        m_configStatus->setText(QStringLiteral("保存状态读取失败，请手动点击“读取配置”确认。"));
    }
}

void MainWindow::onFeatureResult(bool success, bool write, const QByteArray &data, const QString &error)
{
    m_performance->recordFeature(write, success);
    if (write) {
        if (!success) {
            m_configWaitingSave = false;
            m_configPollTimer.stop();
            m_configStatus->setText(QStringLiteral("配置写入失败：%1").arg(error));
            return;
        }
        m_configWaitingSave = true;
        m_configPolls = 0;
        m_configPollTimer.start();
        return;
    }

    if (!success || data.size() < Proto::kConfigXferSize) {
        if (m_configWaitingSave && ++m_configPolls < 10) {
            m_configPollTimer.start();
            return;
        }
        m_configWaitingSave = false;
        m_configPollTimer.stop();
        m_configStatus->setText(QStringLiteral("配置读取失败：%1").arg(error.isEmpty() ? QStringLiteral("报告长度错误") : error));
        return;
    }

    const QByteArray cfg = data.mid(1, Proto::kConfigSize);
    if (!Proto::validConfig(cfg)) {
        m_configWaitingSave = false;
        m_configStatus->setText(QStringLiteral("设备配置校验失败，请确认已烧录支持配置的新固件。"));
        return;
    }
    const int status = quint8(cfg.at(3));
    if (m_configWaitingSave && status == 1) {
        if (++m_configPolls >= 30) {
            m_configWaitingSave = false;
            m_configStatus->setText(QStringLiteral("等待 Flash 保存超时，请重新读取确认。"));
        } else {
            m_configPollTimer.start();
        }
        return;
    }

    const bool confirmed = m_configWaitingSave;
    m_configWaitingSave = false;
    m_configPollTimer.stop();
    for (int i = 0; i < 9; ++i) {
        const int index = m_actionCombos.at(i)->findData(Proto::get16(cfg, 4 + i * 2));
        m_actionCombos.at(i)->setCurrentIndex(index >= 0 ? index : 0);
    }
    for (int i = 0; i < 2; ++i) m_sens[i]->setValue(quint8(cfg.at(22 + i)));
    m_deadzone[0]->setValue(Proto::get16(cfg, 24));
    m_deadzone[1]->setValue(Proto::get16(cfg, 26));

    if (confirmed) {
        m_configStatus->setText(status == 2
            ? QStringLiteral("已应用并写入 Flash，断电后配置仍会保留。")
            : QStringLiteral("配置已应用，但 Flash 保存失败；可再次点击“写入并保存”。"));
    } else {
        m_configStatus->setText(QStringLiteral("配置读取成功（灵敏度 %1% / %2%，死区 %3 / %4）。")
            .arg(m_sens[0]->value()).arg(m_sens[1]->value())
            .arg(m_deadzone[0]->value()).arg(m_deadzone[1]->value()));
    }
    appendLog(QStringLiteral("CFG"), m_configStatus->text());
}

void MainWindow::loadSettings()
{
    QSettings s(QStringLiteral("shujian"), QStringLiteral("HidUpper"));
    const int r = s.value(QStringLiteral("rgb/r"), 0).toInt();
    const int g = s.value(QStringLiteral("rgb/g"), 0).toInt();
    const int b = s.value(QStringLiteral("rgb/b"), 0).toInt();
    for (int i = 0; i < 3; ++i) m_sliders.at(i)->setValue(i == 0 ? r : (i == 1 ? g : b));
    applyColor(r, g, b, false);
}

void MainWindow::saveSettings()
{
    QSettings s(QStringLiteral("shujian"), QStringLiteral("HidUpper"));
    s.setValue(QStringLiteral("rgb/r"), m_sliders.at(0)->value());
    s.setValue(QStringLiteral("rgb/g"), m_sliders.at(1)->value());
    s.setValue(QStringLiteral("rgb/b"), m_sliders.at(2)->value());
}

