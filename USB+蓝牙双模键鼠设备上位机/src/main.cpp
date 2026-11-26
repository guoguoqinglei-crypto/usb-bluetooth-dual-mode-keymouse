#include <QApplication>
#include <QFont>
#include <QStyleFactory>
#include <QTimer>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setApplicationName(QStringLiteral("USB 键鼠控制台"));
    QApplication::setOrganizationName(QStringLiteral("MinZhan"));
    QApplication::setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));

    MainWindow w;
    w.setWindowTitle(QStringLiteral("USB 键鼠控制台  |  自定义 HID 双向交互"));
    w.resize(1180, 820);
    w.setMinimumSize(1080, 740);
    w.show();

    /* 供交付前自动截图检查使用；正常运行不带此参数，不影响设备功能。 */
    const QStringList args = app.arguments();
    const int shot = args.indexOf(QStringLiteral("--screenshot"));
    if (shot >= 0 && shot + 1 < args.size()) {
        const QString path = args.at(shot + 1);
        QTimer::singleShot(1600, &w, [&app, &w, path]() {
            app.exit(w.grab().save(path) ? 0 : 2);
        });
    }
    return app.exec();
}
