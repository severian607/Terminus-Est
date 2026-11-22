// terminus-bar-menu-blur: Hyprland plugin that gives the Terminus-Bar control-center layer a gentler blur than the
// global one, by overriding the blur settings only for the duration of that surface's draw call.
// Tied to one Hyprland commit's internal headers; PLUGIN_INIT refuses to load on any other.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>

#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
constexpr std::string_view SUPPORTED_COMMIT = "11cbc6c577b84ffb1edf4df3fc5b6128c3ae6947"; // Hyprland v0.57.2
constexpr std::string_view DRAW_SIGNATURE =
    "Render::IElementRenderer::drawSurface(Render::CRenderContext&, Hyprutils::Memory::CWeakPointer<CSurfacePassElement>, Hyprutils::Math::CRegion const&)";

constexpr std::string_view MENU_NAMESPACE = "terminusbar-control-center";

// Blur used for the menu surface: lighter than the global look, with no colour or noise adjustments.
constexpr Config::INTEGER MENU_BLUR_SIZE = 3;
constexpr Config::INTEGER MENU_BLUR_PASSES = 2;
constexpr Config::FLOAT MENU_BLUR_BRIGHTNESS = 1;
constexpr Config::FLOAT MENU_BLUR_CONTRAST = 1;
constexpr Config::FLOAT MENU_BLUR_NOISE = 0;
constexpr Config::FLOAT MENU_BLUR_VIBRANCY = 0;

// A native member-function trampoline receives the renderer's this pointer first.
using DrawSurface = void (*)(Render::IElementRenderer*, Render::CRenderContext&, WP<CSurfacePassElement>, const CRegion&);

HANDLE g_handle = nullptr;
CFunctionHook* g_hook = nullptr; // Owned by Hyprland's hook manager, never delete directly.

[[noreturn]] void fail(const std::string& reason) {
    throw std::runtime_error("terminus-bar-menu-blur: " + reason +
        ". Rebuild with make -B -C ~/Projects/Terminus-Est/modules/terminus-bar-menu-blur using the running Hyprland's headers/compiler; "
        "a different Hyprland revision also requires reviewing this plugin's render hook.");
}

template <typename T>
void bindChecked(CConfigValue<T>& value, const std::string& name) {
    const auto option = Config::mgr()->getConfigValue(name);
    if (!option.dataptr || !*option.dataptr || !option.type || *option.type != typeid(T))
        fail("missing or incompatible config value " + name);

    value.bind(name);
    if (!value.good() || !value.ptr())
        fail("could not bind config value " + name);
}

struct BlurConfig {
    CConfigValue<Config::INTEGER> size;
    CConfigValue<Config::INTEGER> passes;
    CConfigValue<Config::FLOAT> brightness;
    CConfigValue<Config::FLOAT> contrast;
    CConfigValue<Config::FLOAT> noise;
    CConfigValue<Config::FLOAT> vibrancy;

    BlurConfig() {
        bindChecked(size, "decoration:blur:size");
        bindChecked(passes, "decoration:blur:passes");
        bindChecked(brightness, "decoration:blur:brightness");
        bindChecked(contrast, "decoration:blur:contrast");
        bindChecked(noise, "decoration:blur:noise");
        bindChecked(vibrancy, "decoration:blur:vibrancy");
    }
};

// Empty until ABI checks pass. Registered CConfigValue objects follow reload rebinding;
// raw primitive pointers are obtained only for the current synchronous draw.
std::optional<BlurConfig> g_config;

template <typename T>
class ScopedOverride {
  public:
    ScopedOverride(T& value, T replacement) noexcept : m_value(value), m_saved(value) {
        m_value = replacement;
    }
    ~ScopedOverride() noexcept {
        m_value = m_saved;
    }
    ScopedOverride(const ScopedOverride&) = delete;
    ScopedOverride& operator=(const ScopedOverride&) = delete;

  private:
    T& m_value;
    T m_saved;
};

void drawSurface(Render::IElementRenderer* renderer, Render::CRenderContext& context, WP<CSurfacePassElement> element, const CRegion& damage) {
    const auto original = reinterpret_cast<DrawSurface>(g_hook->m_original);
    if (!element || !element->m_data.pLS || element->m_data.pLS->m_namespace != MENU_NAMESPACE || !element->m_data.blur) {
        original(renderer, context, std::move(element), damage);
        return;
    }

    // shouldBlur(layer) already honors global blur:enabled and the client's blur
    // request. Do not force blur or change any surface/content opacity here.
    auto& data = element->m_data;
    const ScopedOverride size{*g_config->size.ptr(), MENU_BLUR_SIZE};
    const ScopedOverride passes{*g_config->passes.ptr(), MENU_BLUR_PASSES};
    const ScopedOverride brightness{*g_config->brightness.ptr(), MENU_BLUR_BRIGHTNESS};
    const ScopedOverride contrast{*g_config->contrast.ptr(), MENU_BLUR_CONTRAST};
    const ScopedOverride noise{*g_config->noise.ptr(), MENU_BLUR_NOISE};
    const ScopedOverride vibrancy{*g_config->vibrancy.ptr(), MENU_BLUR_VIBRANCY};
    const ScopedOverride discardOpacity{data.discardOpacity, 0.F};
    const ScopedOverride blockBlurOptimization{data.blockBlurOptimization, true};

    original(renderer, context, std::move(element), damage);
}

void release() {
    if (g_hook)
        HyprlandAPI::removeFunctionHook(g_handle, g_hook);
    g_hook = nullptr;
    // Destruction unregisters every config reference before dlclose/config reload.
    g_config.reset();
    g_handle = nullptr;
}
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    const char* serverHash = __hyprland_api_get_hash();
    const char* clientHash = __hyprland_api_get_client_hash();
    if (!serverHash || !clientHash || std::string_view{serverHash} != clientHash)
        fail("full Hyprland API/dependency hash mismatch");

    // Do not touch internal config or renderer ABI before both version gates.
    const auto version = HyprlandAPI::getHyprlandVersion(handle);
    if (std::string_view{GIT_COMMIT_HASH} != SUPPORTED_COMMIT || version.hash != GIT_COMMIT_HASH)
        fail("unsupported Hyprland commit (headers " + std::string{GIT_COMMIT_HASH} + ", runtime " + version.hash + ")");

    const auto matches = HyprlandAPI::findFunctionsByName(handle, "drawSurface");
    const SFunctionMatch* target = nullptr;
    for (const auto& match : matches) {
        if (match.demangled != DRAW_SIGNATURE)
            continue;
        if (target)
            fail("ambiguous drawSurface signature");
        target = &match;
    }
    if (!target || !target->address)
        fail("required drawSurface signature was not found");
    if (!Config::mgr())
        fail("config manager is unavailable");

    PLUGIN_DESCRIPTION_INFO description{"terminus-bar-menu-blur", "Gentle draw-scoped blur for the Terminus-Bar menu", "Terminus-Est", "1.0.0"};
    g_handle = handle;
    try {
        g_config.emplace();
        g_hook = HyprlandAPI::createFunctionHook(handle, target->address, reinterpret_cast<void*>(&drawSurface));
        if (!g_hook || !g_hook->hook())
            fail("could not install drawSurface hook");
    } catch (...) {
        // Hyprland does not call PLUGIN_EXIT when initialization throws.
        release();
        throw;
    }
    return description;
}

APICALL EXPORT void PLUGIN_EXIT() {
    release();
}
