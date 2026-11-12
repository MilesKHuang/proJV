#pragma once

#include <cstddef>
#include <string>

namespace log_files {

// <logDir>/proJV-YYYY-MM-DD.log for today, local time. The date in the file
// name is the whole rotation policy: no background thread, no size check, and
// `ls` tells you which day a file belongs to.
std::string todayLogPath(const std::string& logDir);

// Delete <logDir>/proJV-YYYY-MM-DD.log files older than keepDays.
//
// Anything that does not match that exact pattern stays: crash-*.dmp,
// skill_metrics.log, the old proJV.log and whatever a user dropped there all
// share this directory, and deleting the wrong file is worse than keeping one
// too many. keepDays <= 0 disables pruning entirely.
//
// Returns the number of files removed.
std::size_t pruneOldLogs(const std::string& logDir, int keepDays);

} // namespace log_files
