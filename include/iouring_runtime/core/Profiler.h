#pragma once

// Tracy Profiler wrapper — isolates framework code from direct Tracy dependency.
// When TRACY_ENABLE is defined (via CMake), all macros forward to Tracy.
// Otherwise they expand to no-ops with zero overhead.

#ifdef TRACY_ENABLE

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

// TracyLockable, LockableBase, ZoneScoped, FrameMark, etc. are already
// defined by Tracy.hpp — no need to redefine them here.

#else // TRACY_ENABLE not defined — no-op stubs

// Zone scoping
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedNC(name, color)

// Frame marks
#define FrameMark
#define FrameMarkNamed(name)
#define FrameMarkStart(name)
#define FrameMarkEnd(name)

// Messages
#define TracyMessageL(msg)
#define TracyMessage(msg, len)

// Plots
#define TracyPlot(name, val)

// Thread naming (C API)
#define TracyCSetThreadName(name)

// Lock wrapper: falls back to plain mutex type
#define TracyLockable(type, varname) type varname
#define LockableBase(type) type

#endif // TRACY_ENABLE
