#pragma once

#define WLR_USE_UNSTABLE

#include <deque>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/Context.hpp>

struct box {
    float x = 0, y = 0, w = 0, h = 0;

    //
    Vector2D middle() {
        return Vector2D{x + w / 2.0, y + h / 2.0};
    }
};
struct point2 {
    point2(const Vector2D& v) {
        x = v.x;
        y = v.y;
    }

    point2() {
        x = 0;
        y = 0;
    }

    float x = 0, y = 0;
};

class CTrail : public IHyprWindowDecoration {
  public:
    CTrail(PHLWINDOW);
    virtual ~CTrail();

    virtual SDecorationPositioningInfo getPositioningInfo();

    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);

    virtual void                       draw(Render::CRenderContext& ctx, PHLMONITOR, float const& a, const Render::SWindowRenderPresentation& presentation);

    virtual eDecorationType            getDecorationType();

    virtual void                       updateWindow(PHLWINDOW);

    virtual void                       damageEntire();

    virtual eDecorationLayer           getDecorationLayer();

    virtual uint64_t                   getDecorationFlags();

    virtual std::string                getDisplayName();

  private:
    void                                                              onTick();
    void                                                              renderPass(Render::CRenderContext& ctx, PHLMONITOR pMonitor, const float& a);

    std::deque<std::pair<box, std::chrono::system_clock::time_point>> m_dLastGeoms;

    int                                                               m_iTimer = 0;

    SBoxExtents                                                       m_seExtents;

    PHLWINDOWREF                                                      m_pWindow;

    Vector2D                                                          m_lastWindowPos;
    Vector2D                                                          m_lastWindowSize;

    CBox                                                              m_bLastBox     = {0};
    bool                                                              m_bNeedsDamage = false;

    friend class CTrailPassElement;
    friend void tickTrails();
};

void tickTrails();
