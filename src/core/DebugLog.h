#pragma once

#include <string>

namespace pb {

// Lightweight diagnostic logger. Writes timestamped lines to
// "pbRecorder-debug.log" next to the current working directory and also
// forwards to OutputDebugString. Enabled only when the environment variable
// PBRECORDER_DEBUG is set (to any value), so normal runs stay silent.
//
// Intended for field diagnostics: ask the user to set PBRECORDER_DEBUG=1,
// reproduce once, then send the log file.
void debugLog(const std::string& message);

// Returns true when PBRECORDER_DEBUG is set (cached after first call).
bool debugLogEnabled();

} // namespace pb
