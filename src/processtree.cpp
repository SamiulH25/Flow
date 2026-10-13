#include "processtree.h"

#include <QStringList>
#include <QtGlobal>

#ifdef Q_OS_UNIX
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace ProcessTree {

void prepare(QProcess *process)
{
#ifdef Q_OS_UNIX
    process->setChildProcessModifier([] { ::setpgid(0, 0); });
#else
    Q_UNUSED(process);
#endif
}

void stop(QProcess *process, bool force)
{
    const qint64 pid = process->processId();
    if (pid <= 0)
        return;
#ifdef Q_OS_UNIX
    ::kill(-pid_t(pid), force ? SIGKILL : SIGTERM);
#else
    Q_UNUSED(force);
    // taskkill /T ends the whole tree by parent chain; /F is the only mode Windows has.
    QProcess::execute(QStringLiteral("taskkill"),
                      {QStringLiteral("/T"), QStringLiteral("/F"),
                       QStringLiteral("/PID"), QString::number(pid)});
#endif
}

} // namespace ProcessTree
