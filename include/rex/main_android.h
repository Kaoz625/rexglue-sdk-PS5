// Android helpers the xenia-era POSIX code names (memory_posix.cpp, threading_posix.cpp).
#pragma once
#include <android/api-level.h>
namespace rex {
inline int GetAndroidApiLevel() { return android_get_device_api_level(); }
}  // namespace rex
