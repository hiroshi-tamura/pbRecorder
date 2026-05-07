#include <QApplication>
#include <QStyleFactory>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>
#include <Windows.h>
#include <shellscalingapi.h>

#include "ui/MainWindow.h"

int main(int argc, char* argv[]) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    QApplication app(argc, argv);
    app.setApplicationName("pbRecorder");
    app.setApplicationVersion("0.5.3");
    app.setOrganizationName("pbRecorder");

    app.setStyle(QStyleFactory::create("Fusion"));

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
        "QGroupBox::title { subcontrol-origin: margin; left: 10px; padding: 0 5px; color: #c0c0c0; font-weight: 600; font-size: 13px; }"
        "QSpinBox, QComboBox, QLineEdit { min-height: 24px; padding: 2px 6px; }"
        "QPushButton { min-height: 26px; padding: 4px 12px; }"
        "QPushButton#overwritePresetBtn, QPushButton#saveAsPresetBtn, QPushButton#deletePresetBtn, QPushButton#settingsBtn { font-family: 'Segoe MDL2 Assets'; font-size: 16px; padding: 0; min-width: 32px; min-height: 28px; }"
        "QStatusBar { color: #cccccc; }"
    );

    // Set application icon
    app.setWindowIcon(QIcon(QCoreApplication::applicationDirPath() + "/icon.png"));

    QString outputDir = QCoreApplication::applicationDirPath() + "/Output";
    QDir().mkpath(outputDir);

    MainWindow mainWindow;
    mainWindow.show();

    const QStringList args = app.arguments();
    const int screenshotArg = args.indexOf("--ui-screenshot");
    if (screenshotArg >= 0) {
        const QString path = (screenshotArg + 1 < args.size())
            ? args.at(screenshotArg + 1)
            : QDir::current().filePath("ui-screenshot.png");
        QTimer::singleShot(500, &mainWindow, [&mainWindow, path]() {
            QDir().mkpath(QFileInfo(path).absolutePath());
            mainWindow.grab().save(path);
            mainWindow.close();
        });
    }

    int result = app.exec();
    CoUninitialize();
    return result;
}
