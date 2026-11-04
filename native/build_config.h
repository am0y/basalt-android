#pragma once

#ifndef BASALT_DEV_BRIDGE
#define BASALT_DEV_BRIDGE 0
#endif

// The scheduler/editor path uses the 2.742 child-thread dispatch. Set to 0 to
// restrict the client to direct execution only.
#ifndef BASALT_SCHEDULER_ENABLED
#define BASALT_SCHEDULER_ENABLED 1
#endif

#if !BASALT_DEV_BRIDGE && defined(BASALT_ENABLE_SECURITY_PROBE)
#error "Memory diagnostics cannot be enabled in a release build"
#endif

#if BASALT_DEV_BRIDGE
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "basalt", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "basalt", __VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#define LOGE(...) ((void)0)
#endif
