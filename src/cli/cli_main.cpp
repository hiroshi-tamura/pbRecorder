#include "cli/CliRunner.h"

#include <QCoreApplication>
#include <QStringList>
#include <Windows.h>
#include <objbase.h>

int main(int argc, char* argv[]) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);

    QCoreApplication app(argc, argv);
    app.setApplicationName("pbRecorder");
    app.setApplicationVersion("0.5.7");

    int result = CliRunner::run(app.arguments());

    if (coInitialized) {
        CoUninitialize();
    }
    return result;
}
