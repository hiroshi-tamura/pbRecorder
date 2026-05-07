#include "cli/CliRunner.h"

#include <QCoreApplication>
#include <QStringList>
#include <Windows.h>
#include <objbase.h>

int main(int argc, char* argv[]) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    QCoreApplication app(argc, argv);
    app.setApplicationName("pbRecorder");
    app.setApplicationVersion("0.5.0");

    int result = CliRunner::run(app.arguments());

    CoUninitialize();
    return result;
}
