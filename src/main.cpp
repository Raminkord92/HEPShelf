#include "mainwindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QStyleFactory>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("HEPShelf"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("hepshelf.local"));
    QCoreApplication::setApplicationName(QStringLiteral("HEPShelf"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.9.1"));
    QApplication::setApplicationDisplayName(QStringLiteral("HEPShelf"));

    MainWindow window;
    window.show();
    return app.exec();
}
