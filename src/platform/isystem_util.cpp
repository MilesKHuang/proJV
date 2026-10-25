// proJV -- ISystemUtil singleton (shared by both platforms)
#include "platform/isystem_util.h"

#include <stdexcept>

static ISystemUtil* g_sysUtil = nullptr;

namespace SystemUtil {
ISystemUtil& Instance() {
    // Dereferencing a null singleton used to crash with no explanation. Callers
    // are expected to run after Init(); if one does not, report which contract
    // was broken instead of dying on a null dereference.
    if (!g_sysUtil)
        throw std::runtime_error("SystemUtil::Init() must be called before SystemUtil::Instance()");
    return *g_sysUtil;
}
void Init(ISystemUtil* inst) {
    g_sysUtil = inst;
}
} // namespace SystemUtil
