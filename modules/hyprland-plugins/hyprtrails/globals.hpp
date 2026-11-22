#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <vector>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>

class CEventLoopTimer;
class CTrail;

inline HANDLE PHANDLE = nullptr;

struct SGlobalState {
    CShader             trailShader;
    SP<CEventLoopTimer> tick;
    std::vector<CTrail*> trails;

    struct {
        SP<Config::Values::CFloatValue> bezierStep;
        SP<Config::Values::CIntValue>   pointsPerStep, historyPoints, historyStep;
        SP<Config::Values::CColorValue> color;
    } config;
};

inline UP<SGlobalState> g_pGlobalState;
