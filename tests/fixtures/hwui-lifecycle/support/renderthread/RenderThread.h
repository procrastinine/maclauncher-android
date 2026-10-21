#pragma once
#include "utils/Macros.h"
namespace android::uirenderer::renderthread {
class RenderThread { public: static auto getOnStartHook() -> void(*)(const char*) { return nullptr; } };
}
