#ifndef DASHBOARD_H
#define DASHBOARD_H

#include <QFrame>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStyle>
#include <QString>

/* 两个上位机共用的轻量界面组件。只负责展示，不接触设备数据与通信线程。 */
namespace Dashboard
{

inline QFrame *makeHero(const QString &title, const QString &subtitle,
                        QLabel *status, QWidget *parent)
{
    auto *hero = new QFrame(parent);
    hero->setObjectName(QStringLiteral("hero"));
    auto *lay = new QHBoxLayout(hero);
    lay->setContentsMargins(22, 14, 18, 14);
    lay->setSpacing(18);

    auto *textLay = new QVBoxLayout;
    textLay->setSpacing(2);
    auto *caption = new QLabel(QStringLiteral("MINZHAN  ·  DEVICE CONSOLE"), hero);
    caption->setObjectName(QStringLiteral("heroCaption"));
    auto *titleLabel = new QLabel(title, hero);
    titleLabel->setObjectName(QStringLiteral("heroTitle"));
    auto *subLabel = new QLabel(subtitle, hero);
    subLabel->setObjectName(QStringLiteral("heroSubtitle"));
    textLay->addWidget(caption);
    textLay->addWidget(titleLabel);
    textLay->addWidget(subLabel);
    lay->addLayout(textLay, 1);

    status->setObjectName(QStringLiteral("statusPill"));
    status->setAlignment(Qt::AlignCenter);
    status->setMinimumSize(118, 34);
    lay->addWidget(status, 0, Qt::AlignVCenter);
    return hero;
}

class MetricCard : public QFrame
{
public:
    MetricCard(const QString &title, const QString &hint, QWidget *parent = nullptr)
        : QFrame(parent)
    {
        setObjectName(QStringLiteral("metricCard"));
        setMinimumHeight(76);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(14, 8, 14, 8);
        lay->setSpacing(0);
        auto *titleLabel = new QLabel(title, this);
        titleLabel->setObjectName(QStringLiteral("metricTitle"));
        m_value = new QLabel(QStringLiteral("—"), this);
        m_value->setObjectName(QStringLiteral("metricValue"));
        auto *hintLabel = new QLabel(hint, this);
        hintLabel->setObjectName(QStringLiteral("metricHint"));
        lay->addWidget(titleLabel);
        lay->addWidget(m_value);
        lay->addWidget(hintLabel);
    }

    void setValue(const QString &value)
    {
        if (m_value->text() != value) m_value->setText(value);
    }

private:
    QLabel *m_value = nullptr;
};

class AssessmentBar : public QFrame
{
public:
    explicit AssessmentBar(QWidget *parent = nullptr) : QFrame(parent)
    {
        setObjectName(QStringLiteral("assessmentBar"));
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(14, 8, 12, 8);
        lay->setSpacing(10);

        auto *icon = new QLabel(QStringLiteral("考核助手"), this);
        icon->setObjectName(QStringLiteral("assessmentTitle"));
        m_summary = new QLabel(QStringLiteral("等待设备数据"), this);
        m_summary->setObjectName(QStringLiteral("assessmentSummary"));
        m_progress = new QProgressBar(this);
        m_progress->setRange(0, 5);
        m_progress->setValue(0);
        m_progress->setTextVisible(false);
        m_progress->setMinimumWidth(130);
        m_progress->setMaximumWidth(220);
        m_details = new QPushButton(QStringLiteral("查看结果"), this);
        m_test = new QPushButton(QStringLiteral("RGB 通道自检"), this);
        m_export = new QPushButton(QStringLiteral("导出报告"), this);

        lay->addWidget(icon);
        lay->addWidget(m_summary, 1);
        lay->addWidget(m_progress);
        lay->addWidget(m_details);
        lay->addWidget(m_test);
        lay->addWidget(m_export);
    }

    void setState(int done, int total, const QString &summary)
    {
        m_progress->setMaximum(total);
        m_progress->setValue(done);
        const QString text = QStringLiteral("%1  ·  已完成 %2/%3 项").arg(summary).arg(done).arg(total);
        if (m_summary->text() != text) m_summary->setText(text);
    }

    QPushButton *detailsButton() const { return m_details; }
    QPushButton *testButton() const { return m_test; }
    QPushButton *exportButton() const { return m_export; }

private:
    QLabel *m_summary = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_details = nullptr;
    QPushButton *m_test = nullptr;
    QPushButton *m_export = nullptr;
};

inline QString styleSheet(const QString &accent, const QString &accentDark)
{
    return QStringLiteral(
        "#root{background:#F2F5F7;}"
        "QWidget{font-family:'Microsoft YaHei UI';font-size:13px;color:#243B53;}"
        "#hero{background:#102A43;border-radius:10px;}"
        "#heroCaption{color:#8FB3CC;font-size:10px;font-weight:600;}"
        "#heroTitle{color:#F5F9FC;font-size:23px;font-weight:700;}"
        "#heroSubtitle{color:#C4D5E2;font-size:12px;}"
        "#statusPill{background:#1D405E;border:1px solid #3B5D77;border-radius:17px;"
        "  color:#D7E6EF;font-weight:600;padding:4px 10px;}"
        "#metricCard{background:#FFFFFF;border:1px solid #D9E2EC;border-radius:9px;}"
        "#metricTitle{color:#627D98;font-size:11px;}"
        "#metricValue{color:#102A43;font-size:22px;font-weight:700;}"
        "#metricHint{color:#718096;font-size:10px;}"
        "#assessmentBar{background:#E8F0F4;border:1px solid #CBDCE5;border-radius:9px;}"
        "#assessmentTitle{color:#102A43;font-weight:700;}"
        "#assessmentSummary{color:#486581;font-size:12px;}"
        "QGroupBox{background:#FFFFFF;border:1px solid #D9E2EC;border-radius:9px;"
        "  margin-top:17px;padding-top:11px;font-weight:600;color:#334E68;}"
        "QGroupBox::title{subcontrol-origin:margin;left:12px;padding:0 5px;}"
        "QTabWidget::pane{border:1px solid #D9E2EC;border-radius:8px;background:#F7FAFC;}"
        "QTabBar::tab{background:#E4EBF0;color:#486581;padding:8px 22px;"
        "  border:1px solid #CBD8E2;border-bottom:none;border-top-left-radius:6px;border-top-right-radius:6px;}"
        "QTabBar::tab:selected{background:%1;color:#FFFFFF;border-color:%1;font-weight:600;}"
        "QTabBar::tab:hover:!selected{background:#D7E5EC;color:#102A43;}"
        "QPushButton{background:#FFFFFF;border:1px solid #BCCCDC;border-radius:6px;"
        "  padding:5px 12px;min-height:28px;color:#334E68;}"
        "QPushButton:hover{background:#EAF2F7;border-color:%1;color:#102A43;}"
        "QPushButton:pressed{background:#D9E8F0;}"
        "QPushButton:focus{border:2px solid %1;}"
        "QPushButton:disabled{background:#EEF2F5;color:#9FB3C8;border-color:#D9E2EC;}"
        "QPushButton[primary=\"true\"]{background:%1;color:#F7FBFD;border:1px solid %1;font-weight:600;}"
        "QPushButton[primary=\"true\"]:hover{background:%2;border-color:%2;}"
        "QComboBox,QSpinBox,QLineEdit{background:#FFFFFF;border:1px solid #BCCCDC;"
        "  border-radius:6px;padding:5px 8px;min-height:26px;color:#243B53;}"
        "QComboBox:hover,QSpinBox:hover,QLineEdit:hover{border-color:%1;}"
        "QComboBox:focus,QSpinBox:focus,QLineEdit:focus{border:2px solid %1;}"
        "QComboBox QAbstractItemView{background:#FFFFFF;color:#243B53;"
        "  selection-background-color:#DCEBF2;selection-color:#102A43;}"
        "QSlider::groove:horizontal{height:5px;background:#D9E2EC;border-radius:2px;}"
        "QSlider::sub-page:horizontal{height:5px;background:%1;border-radius:2px;}"
        "QSlider::handle:horizontal{width:15px;height:15px;margin:-6px 0;"
        "  border-radius:7px;background:#FFFFFF;border:2px solid %1;}"
        "QSlider::handle:horizontal:hover{background:#E6F4F1;}"
        "QCheckBox{color:#486581;spacing:7px;}"
        "QCheckBox::indicator{width:16px;height:16px;border:1px solid #9FB3C8;"
        "  border-radius:4px;background:#FFFFFF;}"
        "QCheckBox::indicator:hover{border-color:%1;}"
        "QCheckBox::indicator:checked{background:%1;border-color:%1;}"
        "QPlainTextEdit{background:#102A43;color:#D8E7EF;border:1px solid #1D405E;"
        "  border-radius:8px;padding:7px;font-family:Consolas,'Courier New',monospace;"
        "  font-size:11px;selection-background-color:#315A75;}"
        "QProgressBar{background:#CBDCE5;border:none;border-radius:4px;max-height:8px;}"
        "QProgressBar::chunk{background:%1;border-radius:4px;}"
        "QStatusBar{background:#E8EEF2;color:#627D98;border-top:1px solid #D9E2EC;}"
        "QToolTip{background:#102A43;color:#F0F4F8;border:1px solid #3E4C59;padding:5px;}")
        .arg(accent, accentDark);
}

} /* namespace Dashboard */

#endif /* DASHBOARD_H */
