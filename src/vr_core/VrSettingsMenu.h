#pragma once
// src/vr_core/VrSettingsMenu.h
// In-headset VR Settings Menu rendered onto a CPU-updated RGBA/BGRA buffer for an OpenXR quad layer.
// Supports right-controller laser aiming (point + trigger drag/click) and thumbstick + A/B navigation.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vr {

struct ControllerInputState;

struct VrMenuUpdateResult {
    bool closeRequested    = false;
    bool recenterRequested = false;
};

class VrSettingsMenu {
public:
    static constexpr uint32_t kWidth  = 880;
    static constexpr uint32_t kHeight = 980;

    void SetOpen(bool open);
    bool IsOpen() const { return m_open; }

    // Processes pointer ray (u,v in [0,1] from right controller) and controller buttons/sticks.
    VrMenuUpdateResult Update(const ControllerInputState& input,
                              bool hasRayHit, float rayU, float rayV);

    void MarkDirty() { m_dirty = true; }
    bool IsDirty() const { return m_dirty; }

    // Re-renders the panel pixels if dirty and returns true when outPixels was updated.
    bool RenderIfDirty(bool bgra, std::vector<uint8_t>& outPixels);

private:
    void SetStatus(const char* msg);
    void AdjustRow(int row, int dir);
    void ActivateRow(int row, float clickU);
    void SetSliderFromBarU(int row, float normT);

    bool m_open         = false;
    bool m_dirty        = true;
    int  m_selectedRow  = 0;
    bool m_hasCursor    = false;
    float m_cursorU     = 0.5f;
    float m_cursorV     = 0.5f;
    int   m_dragSlider  = -1;

    bool m_prevTrigger  = false;
    bool m_prevJump     = false;
    bool m_prevMenu     = false;
    int  m_prevNavY     = 0;
    int  m_prevNavX     = 0;
    std::chrono::steady_clock::time_point m_nextRepeatY{};
    std::chrono::steady_clock::time_point m_nextRepeatX{};

    std::string m_statusText;
    std::chrono::steady_clock::time_point m_statusUntil{};
};

} // namespace vr
