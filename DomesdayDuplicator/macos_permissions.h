#pragma once
#ifdef __APPLE__
#include <functional>
void requestMicrophonePermission(std::function<void(bool)> callback);
#endif
