#include "dashboard.hpp"
#include <QApplication>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setApplicationName("Telemetry Monitor");
    Dashboard window;
    window.show();
    return application.exec();
}
