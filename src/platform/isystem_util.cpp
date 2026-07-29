// proJV -- ISystemUtil singleton (shared by both platforms)
#include "platform/isystem_util.h"

static ISystemUtil* g_sysUtil = nullptr;

namespace SystemUtil {
    ISystemUtil& Instance() { return *g_sysUtil; }
    void Init(ISystemUtil* inst) { g_sysUtil = inst; }
}
