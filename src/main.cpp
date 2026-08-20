#include <QApplication>

#include "ui/MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("DialogVideoStudio"));
    QCoreApplication::setApplicationName(QStringLiteral("DialogVideoStudio"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    dvs::MainWindow window;
    window.show();
    return app.exec();
}
