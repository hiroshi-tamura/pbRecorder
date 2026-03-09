#pragma once
#include <QStringList>

class CliRunner {
public:
    // CLIモードかどうか判定
    static bool isCliMode(const QStringList& args);

    // CLI実行。終了コード返す
    static int run(const QStringList& args);

    // --list-* コマンドの処理
    static int listMonitors();
    static int listWindows();
    static int listAudioDevices();

private:
    static void printUsage();
};
