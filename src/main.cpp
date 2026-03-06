#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <Windows.h>
#include <shellscalingapi.h>

#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    // Enable Per-Monitor DPI Awareness V2
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Initialize COM for MF/WASAPI
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    QApplication app(argc, argv);
    app.setApplicationName("pbRecorder");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("pbRecorder");

    // Use Fusion style for consistent look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Dark palette
    QPalette darkPalette;
    darkPalette.setColor(QPalette::Window, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    darkPalette.setColor(QPalette::Base, QColor(35, 35, 35));
    darkPalette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ToolTipBase, QColor(25, 25, 25));
    darkPalette.setColor(QPalette::ToolTipText, Qt::white);
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::Button, QColor(53, 53, 53));
    darkPalette.setColor(QPalette::ButtonText, Qt::white);
    darkPalette.setColor(QPalette::BrightText, Qt::red);
    darkPalette.setColor(QPalette::Link, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    darkPalette.setColor(QPalette::HighlightedText, Qt::black);
    darkPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(127, 127, 127));
    darkPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(127, 127, 127));
    app.setPalette(darkPalette);

    app.setStyleSheet(
        "QToolTip { color: #ffffff; background-color: #2a82da; border: 1px solid white; }"
        "QGroupBox { border: 1px solid #555; border-radius: 4px; margin-top: 1em; padding-top: 0.5em; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; }"
    );

    // Create default Output folder next to exe
    QString outputDir = QCoreApplication::applicationDirPath() + "/Output";
    QDir().mkpath(outputDir);

    MainWindow mainWindow;
    mainWindow.show();

    int result = app.exec();

    CoUninitialize();
    return result;
}
