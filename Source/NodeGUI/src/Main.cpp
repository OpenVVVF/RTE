#include "MainWindow.h"

#include <QApplication>

#include <iostream>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NodeGUI"));
    app.setOrganizationName(QStringLiteral("RTE"));

    NodeGUI::MainWindow window;
    window.showNormal();

    if (argc > 1) {
        if (!window.OpenGraph(argv[1])) {
            std::cerr << "Could not open graph: " << argv[1] << std::endl;
            return 1;
        }
    }

    return app.exec();
}
