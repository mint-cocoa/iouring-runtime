#pragma once

// Tracy profiler wrapper. runtime code includes this header instead of taking a
// direct dependency on Tracy headers. When TRACY_ENABLE is not defined, the
// instrumentation macros compile to no-ops.

#ifdef TRACY_ENABLE

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#else

#define ZoneScoped
#define ZoneScopedC(color)
#define ZoneScopedN(name)
#define ZoneScopedNC(name, color)

#define FrameMark
#define FrameMarkNamed(name)
#define FrameMarkStart(name)
#define FrameMarkEnd(name)

#define TracyMessageL(msg)
#define TracyMessage(msg, len)

#define TracyPlot(name, val)

#define TracyCSetThreadName(name)

#define TracyLockable(type, varname) type varname
#define TracyLockableN(type, varname, desc) type varname
#define LockableBase(type) type
#define LockMark(varname)
#define LockableName(varname, name, size)

#endif
