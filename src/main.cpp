#include "PackManagerWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("PackManager"));
    QCoreApplication::setOrganizationName(QStringLiteral("PackManager"));
    QCoreApplication::setApplicationVersion(QStringLiteral(PACKMANAGER_VERSION));
    PackManagerWindow window;
    window.show();
    return application.exec();
}
