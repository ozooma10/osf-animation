#pragma once

#ifndef OSF_ENABLE_PROFILING
#define OSF_ENABLE_PROFILING 0
#endif

#if OSF_ENABLE_PROFILING
#include <tracy/Tracy.hpp>

#define OSF_PROFILE_SCOPE() ZoneScoped
#define OSF_PROFILE_SCOPE_N(a_name) ZoneScopedN(a_name)
#define OSF_PROFILE_PLOT(a_name, a_value) TracyPlot(a_name, static_cast<double>(a_value))
#else
#define OSF_PROFILE_SCOPE() ((void)0)
#define OSF_PROFILE_SCOPE_N(a_name) ((void)0)
#define OSF_PROFILE_PLOT(a_name, a_value) ((void)0)
#endif
