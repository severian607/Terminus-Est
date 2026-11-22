#include "TrailPassElement.hpp"
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include "trail.hpp"

CTrailPassElement::CTrailPassElement(const CTrailPassElement::STrailData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> CTrailPassElement::draw(Render::CRenderContext& ctx) {
    data.deco->renderPass(ctx, ctx.m_data.pMonitor.lock(), data.a);
    return {};
}

bool CTrailPassElement::needsLiveBlur(Render::CRenderContext&) {
    return false;
}

bool CTrailPassElement::needsPrecomputeBlur(Render::CRenderContext&) {
    return false;
}

std::optional<CBox> CTrailPassElement::boundingBox(Render::CRenderContext& ctx) {
    // The trail extends beyond the window; cover the whole monitor so occlusion never drops it.
    return CBox{0, 0, ctx.m_data.pMonitor->m_size.x, ctx.m_data.pMonitor->m_size.y};
}
