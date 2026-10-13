#pragma once

#include <QProcess>

// A yt-dlp run starts ffmpeg and more, so stopping it has to stop all of them.
namespace ProcessTree {

// Call before start(). On POSIX this gives the child its own process group.
void prepare(QProcess *process);

// Ends the child and everything it started. `force` skips the polite request
// (SIGTERM) on POSIX; on Windows both forms terminate the tree.
void stop(QProcess *process, bool force = false);

} // namespace ProcessTree
