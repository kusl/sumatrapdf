
#include "SimpleLog.h"
#include "Vec.h"

namespace Log {

static Vec<Logger*> *g_loggers;
static CRITICAL_SECTION g_logCs;

void Initialize()
{
    g_loggers = new Vec<Logger*>();
    InitializeCriticalSection(&g_logCs);
}

void Destroy()
{
    DeleteVecMembers(*g_loggers);
    DeleteCriticalSection(&g_logCs);
    delete g_loggers;
    g_loggers = NULL;
}

// Note: unless you remove the logger with RemoveLogger(), we own the Logger
// and it'll be deleted when Log::Destroy() is called.
void AddLogger(Logger *logger)
{        
    ScopedCritSec cs(&g_logCs);
    g_loggers->Append(logger);
}

void RemoveLogger(Logger *logger)
{
    ScopedCritSec cs(&g_logCs);
    g_loggers->Remove(logger);
}

void Log(char *s, bool takeOwnership)
{
    if (0 == g_loggers->Count())
        return;

    ScopedCritSec cs(&g_logCs);
    for (size_t i=0; i<g_loggers->Count(); i++)
    {
        g_loggers->At(i)->Log(s, takeOwnership);
    }
}

void LogFmt(char *fmt, ...)
{
    if (0 == g_loggers->Count())
        return;

    va_list args;
    va_start(args, fmt);
    char *s = Str::FmtV(fmt, args);
    Log(s, true);
    va_end(args);
}

} // namespace Log

