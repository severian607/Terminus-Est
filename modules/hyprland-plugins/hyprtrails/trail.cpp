#include "trail.hpp"

#include <algorithm>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/memory/Casts.hpp>
using namespace Hyprutils::Memory;
using namespace Render::GL;
#include "globals.hpp"
#include "TrailPassElement.hpp"

void CTrail::onTick() {
    const auto HISTORYSTEP   = g_pGlobalState->config.historyStep->value();
    const auto HISTORYPOINTS = g_pGlobalState->config.historyPoints->value();

    if (!validMapped(m_pWindow))
        return;

    m_iTimer++;

    const auto PWINDOW = m_pWindow.lock();

    if (m_iTimer > HISTORYSTEP) {
        m_dLastGeoms.push_front({box{(float)PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, (float)PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y, (float)PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x,
                                     (float)PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y},
                                 std::chrono::system_clock::now()});
        while (m_dLastGeoms.size() > HISTORYPOINTS)
            m_dLastGeoms.pop_back();

        m_iTimer = 0;
    }

    if (m_bNeedsDamage) {
        g_pHyprRenderer->damageBox(m_bLastBox);
        m_bNeedsDamage = false;
    }
}

CTrail::CTrail(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow), m_pWindow(pWindow) {
    m_lastWindowPos  = pWindow->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    m_lastWindowSize = pWindow->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    if (g_pGlobalState)
        g_pGlobalState->trails.push_back(this);
}

CTrail::~CTrail() {
    damageEntire();

    if (g_pGlobalState)
        std::erase(g_pGlobalState->trails, this);
}

SDecorationPositioningInfo CTrail::getPositioningInfo() {
    return {DECORATION_POSITION_ABSOLUTE};
}

void CTrail::onPositioningReply(const SDecorationPositioningReply& reply) {
    ; // ignored
}

void scaleBox2(box& box, float coeff) {
    float hwl = (box.w - (box.w * coeff)) / 2.0;
    float hhl = (box.h - (box.h * coeff)) / 2.0;

    box.w *= coeff;
    box.h *= coeff;
    box.x += hwl;
    box.y += hhl;
}

Vector2D vecForT(const Vector2D& a, const Vector2D& b, const float& t) {
    const Vector2D vec_PQ = b - a;
    return Vector2D{a + vec_PQ * t};
}

Vector2D vecForBezierT(const float& t, const std::vector<Vector2D>& verts) {
    std::vector<Vector2D> pts;

    for (size_t vertexIndex = 0; vertexIndex < verts.size() - 1; vertexIndex++) {
        Vector2D p = verts[vertexIndex];
        pts.push_back(vecForT(p, verts[vertexIndex + 1], t));
    }

    if (pts.size() > 1)
        return vecForBezierT(t, pts);
    else
        return pts[0];
}

void CTrail::draw(Render::CRenderContext& ctx, PHLMONITOR pMonitor, const float& a, const Render::SWindowRenderPresentation& presentation) {
    if (!validMapped(m_pWindow))
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    auto data = CTrailPassElement::STrailData{this, a};
    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CTrailPassElement>(data));
}

void CTrail::renderPass(Render::CRenderContext& ctx, PHLMONITOR pMonitor, const float& a) {
    const auto         PWINDOW = m_pWindow.lock();

    const float        BEZIERSTEP    = g_pGlobalState->config.bezierStep->value();
    const int          POINTSPERSTEP = g_pGlobalState->config.pointsPerStep->value();
    const auto         COLORVAL      = g_pGlobalState->config.color->value();

    const CHyprColor   COLOR = CHyprColor(static_cast<uint64_t>(COLORVAL));

    if (m_dLastGeoms.size() < 2)
        return;

    box thisbox =
        box{(float)PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, (float)PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y, (float)PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x, (float)PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y};
    CBox wlrbox = {thisbox.x - pMonitor->m_position.x, thisbox.y - pMonitor->m_position.y, thisbox.w, thisbox.h};
    wlrbox.scale(pMonitor->m_scale).round();

    g_pHyprOpenGL->scissor(ctx, nullptr); // allow the entire window and stencil to render
    glStencilMask(0xFF);
    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);

    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

    glStencilFunc(GL_ALWAYS, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    g_pHyprOpenGL->renderRect(ctx, wlrbox, CHyprColor(0, 0, 0, 0), {.round = sc<int>(PWINDOW->presentation().rounding() * pMonitor->m_scale), .roundingPower = PWINDOW->presentation().roundingPower()});
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glStencilFunc(GL_NOTEQUAL, 1, -1);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    CBox   monbox = {0, 0, ctx.m_data.pMonitor->m_transformedSize.x, ctx.m_data.pMonitor->m_transformedSize.y};

    Mat3x3 glMatrix = g_pHyprRenderer->projectBoxToTarget(ctx, monbox, Math::wlTransformToHyprutils(Math::invertTransform(WL_OUTPUT_TRANSFORM_NORMAL)));

    g_pHyprOpenGL->blend(true);

    glUseProgram(g_pGlobalState->trailShader.program());

    glMatrix.transpose();
    g_pGlobalState->trailShader.setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, glMatrix.getMatrix());

    std::vector<point2>   points;
    std::vector<Vector2D> bezierPts;
    std::vector<Vector2D> pointsForBezier;
    std::vector<float>    agesForBezier;

    float                 originalCoeff = 50;

    auto                  msFrom = [](std::chrono::system_clock::time_point t) -> float {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now() - t).count() / 1000.0;
    };

    auto dist = [&](const point2& a, const point2& b) -> float {
        Vector2D diff = Vector2D{a.x - b.x, a.y - b.y} * pMonitor->m_size;
        return std::sqrt(diff.x * diff.x + diff.y * diff.y);
    };

    float    msMaxToLast = msFrom(m_dLastGeoms.back().second);

    float    dists[2] = {0, 0};

    Vector2D mainVec      = {originalCoeff / pMonitor->m_size.x, originalCoeff / pMonitor->m_size.y};
    Vector2D windowMiddle = PWINDOW->middle() - pMonitor->m_position;

    points.push_back(
        Vector2D{cos(0) * mainVec.x - sin(0) * mainVec.y + windowMiddle.x / pMonitor->m_size.x, sin(0) * mainVec.x + cos(0) * mainVec.y + windowMiddle.y / pMonitor->m_size.y});
    points.push_back(Vector2D{cos(-M_PI_2) * mainVec.x - sin(-M_PI_2) * mainVec.y + windowMiddle.x / pMonitor->m_size.x,
                              sin(-M_PI_2) * mainVec.x + cos(-M_PI_2) * mainVec.y + windowMiddle.y / pMonitor->m_size.y});
    points.push_back(Vector2D{cos(M_PI_2) * mainVec.x - sin(M_PI_2) * mainVec.y + windowMiddle.x / pMonitor->m_size.x,
                              sin(M_PI_2) * mainVec.x + cos(M_PI_2) * mainVec.y + windowMiddle.y / pMonitor->m_size.y});
    points.push_back(Vector2D{cos(M_PI) * mainVec.x - sin(M_PI) * mainVec.y + windowMiddle.x / pMonitor->m_size.x,
                              sin(M_PI) * mainVec.x + cos(M_PI) * mainVec.y + windowMiddle.y / pMonitor->m_size.y});

    pointsForBezier.push_back(windowMiddle);
    agesForBezier.push_back(0);

    for (size_t i = 0; i < m_dLastGeoms.size(); i += 1) {
        box box = m_dLastGeoms[i].first;
        box.x -= pMonitor->m_position.x;
        box.y -= pMonitor->m_position.y;
        Vector2D middle = {box.x + box.w / 2.0, box.y + box.h / 2.0};

        if (middle == pointsForBezier[pointsForBezier.size() - 1])
            continue;

        pointsForBezier.push_back(middle);
        agesForBezier.push_back(msFrom(m_dLastGeoms[i].second));
    }

    if (pointsForBezier.size() < 3) {
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);

        glStencilMask(-1);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        g_pHyprOpenGL->scissor(ctx, nullptr);
        return;
    }

    float maxAge          = agesForBezier.back();
    float tCoeff          = BEZIERSTEP;
    int   pointsPerBezier = POINTSPERSTEP;
    bezierPts.push_back(vecForBezierT(0, pointsForBezier));
    for (float t = tCoeff; t <= 1.0; t += tCoeff) {
        bezierPts.push_back(vecForBezierT(t, pointsForBezier));

        const Vector2D& lastbezier     = bezierPts.back();
        const Vector2D& lastprevbezier = bezierPts[bezierPts.size() - 2];

        for (size_t i = 1; i < pointsPerBezier + 1; ++i) {
            const float     bezierPointStep = (1.0 / (pointsPerBezier + 2));
            const Vector2D& middle          = vecForT(lastprevbezier, lastbezier, bezierPointStep * (i + 1));
            const Vector2D& lastmiddle      = vecForT(lastprevbezier, lastbezier, bezierPointStep * i);

            Vector2D        vecNormal = {middle.x - lastmiddle.x, middle.y - lastmiddle.y};

            // normalize vec
            float invlen = 1.0 / std::sqrt(vecNormal.x * vecNormal.x + vecNormal.y * vecNormal.y);
            vecNormal.x *= invlen;
            vecNormal.y *= invlen;

            // make sure it points up
            // vecNormal.y = std::abs(vecNormal.y);

            // extend by coeff
            float    ageCoeff  = t * (agesForBezier.size() - 1);
            float    ageFloor  = std::floor(ageCoeff);
            float    ageCeil   = std::ceil(ageCoeff);
            float    approxAge = agesForBezier[sc<int>(ageFloor)] + (agesForBezier[sc<int>(ageCeil)] - agesForBezier[sc<int>(ageFloor)]) * (ageCoeff - ageFloor);
            float    coeff     = originalCoeff * (1.0 - (approxAge / maxAge));
            Vector2D newVec    = {vecNormal.x * coeff / pMonitor->m_size.x, vecNormal.y * coeff / pMonitor->m_size.y};

            if ((newVec.x == 0 && newVec.y == 0) || std::isnan(newVec.x) || std::isnan(newVec.y))
                continue;

            // rotate by 90 and -90 and add middle
            points.push_back(Vector2D{cos(M_PI_2) * newVec.x - sin(M_PI_2) * newVec.y + middle.x / pMonitor->m_size.x,
                                      sin(M_PI_2) * newVec.x + cos(M_PI_2) * newVec.y + middle.y / pMonitor->m_size.y});
            points.push_back(Vector2D{cos(-M_PI_2) * newVec.x - sin(-M_PI_2) * newVec.y + middle.x / pMonitor->m_size.x,
                                      sin(-M_PI_2) * newVec.x + cos(-M_PI_2) * newVec.y + middle.y / pMonitor->m_size.y});
        }
    }

    box thisboxopengl = box{sc<float>((PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - pMonitor->m_position.x) / pMonitor->m_size.x),
                            sc<float>((PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - pMonitor->m_position.y) / pMonitor->m_size.y),
                            sc<float>((PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x) / pMonitor->m_size.x),
                            sc<float>((PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y) / pMonitor->m_size.y)};
    glUniform4f(g_pGlobalState->trailShader.getUniformLocation(SHADER_GRADIENT), thisboxopengl.x, thisboxopengl.y, thisboxopengl.w, thisboxopengl.h);
    glUniform4f(g_pGlobalState->trailShader.getUniformLocation(SHADER_COLOR), COLOR.r, COLOR.g, COLOR.b, COLOR.a);

    CBox transformedBox = monbox;
    transformedBox.transform(Math::wlTransformToHyprutils(Math::invertTransform(ctx.m_data.pMonitor->m_transform)), ctx.m_data.pMonitor->m_transformedSize.x,
                             ctx.m_data.pMonitor->m_transformedSize.y);

    glVertexAttribPointer(g_pGlobalState->trailShader.getUniformLocation(SHADER_POS_ATTRIB), 2, GL_FLOAT, GL_FALSE, 0, (float*)points.data());

    glEnableVertexAttribArray(g_pGlobalState->trailShader.getUniformLocation(SHADER_POS_ATTRIB));

    if (ctx.m_data.clipBox.width != 0 && ctx.m_data.clipBox.height != 0) {
        CRegion damageClip{ctx.m_data.clipBox.x, ctx.m_data.clipBox.y, ctx.m_data.clipBox.width,
                           ctx.m_data.clipBox.height};
        damageClip.intersect(ctx.m_data.damage);

        if (!damageClip.empty()) {
            for (auto& RECT : damageClip.getRects()) {
                g_pHyprOpenGL->scissor(ctx, &RECT);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, points.size());
            }
        }
    } else {
        for (auto& RECT : ctx.m_data.damage.getRects()) {
            g_pHyprOpenGL->scissor(ctx, &RECT);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, points.size());
        }
    }

    glDisableVertexAttribArray(g_pGlobalState->trailShader.getUniformLocation(SHADER_POS_ATTRIB));

    glClearStencil(0);
    glClear(GL_STENCIL_BUFFER_BIT);
    g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);

    glStencilMask(-1);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    g_pHyprOpenGL->scissor(ctx, nullptr);

    // calculate damage
    float minX = 9999999;
    float minY = 9999999;
    float maxX = -9999999;
    float maxY = -9999999;

    for (auto& p : points) {
        if (p.x < minX)
            minX = p.x;
        if (p.y < minY)
            minY = p.y;
        if (p.x > maxX)
            maxX = p.x;
        if (p.y > maxY)
            maxY = p.y;
    }

    // bring back to global coords
    minX *= pMonitor->m_size.x;
    minY *= pMonitor->m_size.y;
    maxX *= pMonitor->m_size.x;
    maxY *= pMonitor->m_size.y;

    m_bLastBox.x      = minX + pMonitor->m_position.x;
    m_bLastBox.y      = minY + pMonitor->m_position.y;
    m_bLastBox.width  = maxX - minX;
    m_bLastBox.height = maxY - minY;

    m_bNeedsDamage = true;
}

eDecorationType CTrail::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CTrail::updateWindow(PHLWINDOW pWindow) {
    m_lastWindowPos  = pWindow->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
    m_lastWindowSize = pWindow->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    damageEntire();
}

void CTrail::damageEntire() {
    CBox dm = {sc<double>(m_lastWindowPos.x - m_seExtents.topLeft.x), sc<double>(m_lastWindowPos.y - m_seExtents.topLeft.y),
               sc<double>(m_lastWindowSize.x + m_seExtents.topLeft.x + m_seExtents.bottomRight.x), sc<double>(m_seExtents.topLeft.y)};
    g_pHyprRenderer->damageBox(dm);
}

void tickTrails() {
    if (!g_pGlobalState)
        return;

    for (auto* const trail : g_pGlobalState->trails) {
        if (trail)
            trail->onTick();
    }
}

eDecorationLayer CTrail::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CTrail::getDecorationFlags() {
    return DECORATION_NON_SOLID;
}

std::string CTrail::getDisplayName() {
    return "Hyprtrails";
}
