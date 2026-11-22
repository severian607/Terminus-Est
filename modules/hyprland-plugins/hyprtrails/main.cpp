#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <any>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/shaders/Shaders.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include "globals.hpp"
#include "shaders.hpp"
#include "trail.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

void onNewWindow(PHLWINDOW PWINDOW) {
    HyprlandAPI::addWindowDecoration(PHANDLE, PWINDOW, makeShared<CTrail>(PWINDOW));
}

static void onTick(SP<CEventLoopTimer> self, void* data) {
    tickTrails();

    const int timeoutMs = g_pHyprRenderer->m_mostHzMonitor ? 1000.0 / g_pHyprRenderer->m_mostHzMonitor->m_refreshRate : 16;
    self->updateTimeout(std::chrono::milliseconds(timeoutMs));
}

void initGlobal() {
    Render::GL::g_pHyprOpenGL->makeEGLCurrent();

    if (!g_pGlobalState->trailShader.createProgram(QUADTRAIL, FRAGTRAIL, true, false))
        throw std::runtime_error("[ht] Failed to create trail shader");

    g_pGlobalState->tick = makeShared<CEventLoopTimer>(std::chrono::milliseconds(1), onTick, nullptr);
    g_pEventLoopManager->addTimer(g_pGlobalState->tick);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        HyprlandAPI::addNotification(PHANDLE, "[ht] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)", CHyprColor{1.0, 0.2, 0.2, 1.0},
                                     5000);
        throw std::runtime_error("[ht] Version mismatch");
    }

    g_pGlobalState = makeUnique<SGlobalState>();
    auto& cfg      = g_pGlobalState->config;
    cfg.bezierStep    = makeShared<Config::Values::CFloatValue>("plugin:hyprtrails:bezier_step", "bezier step", 0.025F);
    cfg.pointsPerStep = makeShared<Config::Values::CIntValue>("plugin:hyprtrails:points_per_step", "points per step", 2);
    cfg.historyPoints = makeShared<Config::Values::CIntValue>("plugin:hyprtrails:history_points", "history points", 20);
    cfg.historyStep   = makeShared<Config::Values::CIntValue>("plugin:hyprtrails:history_step", "history step", 2);
    cfg.color         = makeShared<Config::Values::CColorValue>("plugin:hyprtrails:color", "trail color", 0xffaa00ff);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.bezierStep);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.pointsPerStep);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.historyPoints);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.historyStep);
    HyprlandAPI::addConfigValueV2(PHANDLE, cfg.color);

    static auto P = Event::bus()->m_events.window.open.listen([&](PHLWINDOW w) { onNewWindow(w); });

    initGlobal();

    // add deco to existing windows
    for (auto& w : Desktop::windowState()->windows()) {
        if (w->isHidden() || !validMapped(w))
            continue;

        HyprlandAPI::addWindowDecoration(PHANDLE, w, makeShared<CTrail>(w));
    }

    return {"hyprtrails", "A plugin to add trails behind moving windows", "Vaxry", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (g_pGlobalState && g_pGlobalState->tick) {
        g_pGlobalState->tick->cancel();
        g_pEventLoopManager->removeTimer(g_pGlobalState->tick);
    }

}
