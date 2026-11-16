#include "vr_core/VrSettingsMenu.h"
#include "vr_core/OpenXRManager.h"
#include "common/Config.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace vr {
namespace {

constexpr int kRowCount   = 15;
constexpr int kHeaderH    = 88;
constexpr int kRowStartY  = 104;
constexpr int kRowH       = 51;
constexpr int kPadX       = 44;
constexpr int kBarX0      = 420;
constexpr int kBarX1      = 706;

struct Color {
    uint8_t r, g, b, a;
};

// Minimalist dark charcoal-slate & warm lantern-amber palette matching A Story About My Uncle's UI.
constexpr Color kColBgTop      {14,  15,  18, 232};
constexpr Color kColBgBot      {10,  11,  14, 236};
constexpr Color kColBorder     {255, 255, 255, 36};
constexpr Color kColRule       {255, 255, 255, 30};
constexpr Color kColRowHover   {255, 255, 255, 20};
constexpr Color kColBtnIdle    {255, 255, 255, 10};
constexpr Color kColBtnHover   {255, 255, 255, 28};
constexpr Color kColBtnEdge    {255, 255, 255, 42};
constexpr Color kColAccent     {232, 186, 108, 255};  // Warm amber-gold hover/active accent
constexpr Color kColTextHi     {244, 244, 246, 255};  // Crisp chalk white
constexpr Color kColTextMid    {186, 190, 198, 255};  // Clean neutral silver
constexpr Color kColTextDim    {128, 134, 144, 255};  // Subtle muted gray
constexpr Color kColBarTrack   {255, 255, 255, 34};
constexpr Color kColBarIdle    {205, 210, 218, 230};
constexpr Color kColToggleOn   {232, 186, 108, 255};
constexpr Color kColToggleOff  {136, 142, 152, 255};

float RoundedBoxSdf(float px, float py, float x0, float y0, float w, float h, float rad) {
    const float hx = w * 0.5f - rad;
    const float hy = h * 0.5f - rad;
    const float cx = x0 + w * 0.5f;
    const float cy = y0 + h * 0.5f;
    const float qx = std::fabs(px - cx) - std::max(0.0f, hx);
    const float qy = std::fabs(py - cy) - std::max(0.0f, hy);
    const float outX = std::max(qx, 0.0f);
    const float outY = std::max(qy, 0.0f);
    return std::sqrt(outX * outX + outY * outY) + std::min(std::max(qx, qy), 0.0f) - rad;
}

class Canvas {
public:
    Canvas(uint32_t w, uint32_t h) : m_w(w), m_h(h) {
        m_rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    }

    void FillPanelBackdrop(float cornerRad) {
        for (uint32_t py = 0; py < m_h; ++py) {
            const float ty = static_cast<float>(py) / static_cast<float>(m_h);
            const uint8_t br = static_cast<uint8_t>(kColBgTop.r + (kColBgBot.r - kColBgTop.r) * ty);
            const uint8_t bg = static_cast<uint8_t>(kColBgTop.g + (kColBgBot.g - kColBgTop.g) * ty);
            const uint8_t bb = static_cast<uint8_t>(kColBgTop.b + (kColBgBot.b - kColBgTop.b) * ty);
            const uint8_t ba = static_cast<uint8_t>(kColBgTop.a + (kColBgBot.a - kColBgTop.a) * ty);
            for (uint32_t px = 0; px < m_w; ++px) {
                const float sd = RoundedBoxSdf(px + 0.5f, py + 0.5f, 0.0f, 0.0f,
                                               static_cast<float>(m_w), static_cast<float>(m_h), cornerRad);
                const float cov = std::clamp(0.5f - sd, 0.0f, 1.0f);
                if (cov <= 0.0f) continue;
                SetRawBgra(px, py, Color{br, bg, bb, static_cast<uint8_t>(ba * cov)});
                if (sd > -1.75f && sd <= 0.5f) {
                    const float rim = std::clamp(1.0f - std::fabs(sd + 0.65f) / 1.1f, 0.0f, 1.0f);
                    Color edge = kColBorder;
                    edge.a = static_cast<uint8_t>(edge.a * rim * cov);
                    BlendPixel(static_cast<int>(px), static_cast<int>(py), edge);
                }
            }
        }
    }

    void FillRect(int x, int y, int w, int h, Color c) {
        const int x0 = std::max(0, x);
        const int y0 = std::max(0, y);
        const int x1 = std::min(static_cast<int>(m_w), x + w);
        const int y1 = std::min(static_cast<int>(m_h), y + h);
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                BlendPixel(px, py, c);
            }
        }
    }

    void FillRoundedRect(float x, float y, float w, float h, float rad, Color fill, float borderW = 0.0f, Color borderCol = {}) {
        const int x0 = std::max(0, static_cast<int>(std::floor(x - 1.5f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(y - 1.5f)));
        const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(x + w + 1.5f)));
        const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(y + h + 1.5f)));
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float sd = RoundedBoxSdf(px + 0.5f, py + 0.5f, x, y, w, h, rad);
                if (sd > 0.75f) continue;
                const float fillCov = std::clamp(0.5f - sd, 0.0f, 1.0f);
                if (fillCov > 0.0f && fill.a > 0) {
                    Color sc = fill;
                    sc.a = static_cast<uint8_t>(fill.a * fillCov);
                    BlendPixel(px, py, sc);
                }
                if (borderW > 0.05f && borderCol.a > 0) {
                    const float bCov = std::clamp(borderW * 0.5f + 0.5f - std::fabs(sd + borderW * 0.5f), 0.0f, 1.0f);
                    if (bCov > 0.0f) {
                        Color bc = borderCol;
                        bc.a = static_cast<uint8_t>(borderCol.a * bCov);
                        BlendPixel(px, py, bc);
                    }
                }
            }
        }
    }

    void FillCircle(float cx, float cy, float r, Color c) {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.5f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.5f)));
        const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(cx + r + 1.5f)));
        const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(cy + r + 1.5f)));
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float d = std::sqrt((px + 0.5f - cx) * (px + 0.5f - cx) + (py + 0.5f - cy) * (py + 0.5f - cy));
                const float cov = std::clamp(r + 0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    Color sc = c;
                    sc.a = static_cast<uint8_t>(c.a * cov);
                    BlendPixel(px, py, sc);
                }
            }
        }
    }

    enum class Align { Left, Center, Right };

    void QueueText(int x, int y, int w, int h, int pxHeight, bool bold, Align align, Color col, const char* utf8) {
        if (!utf8 || !*utf8) return;
        const int need = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
        if (need <= 1) return;
        std::wstring ws(static_cast<size_t>(need - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws.data(), need);
        m_texts.push_back(TextItem{x, y, w, h, pxHeight, bold, align, col, std::move(ws)});
    }

    void FinalizeTo(bool wantBgra, std::vector<uint8_t>& outPixels) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = static_cast<LONG>(m_w);
        bmi.bmiHeader.biHeight      = -static_cast<LONG>(m_h);
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC screenDC = GetDC(nullptr);
        HDC memDC    = CreateCompatibleDC(screenDC);
        HBITMAP dib  = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            const size_t totalBytes = static_cast<size_t>(m_w) * m_h * 4;
            std::memcpy(bits, m_rgba.data(), totalBytes);
            HGDIOBJ oldBmp = SelectObject(memDC, dib);
            SetBkMode(memDC, TRANSPARENT);

            auto makeFont = [](int pxH, bool bold) {
                return CreateFontW(-pxH, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            };
            HFONT fTitle = makeFont(28, true);
            HFONT fSub   = makeFont(16, false);
            HFONT fRow   = makeFont(20, false);
            HFONT fRowB  = makeFont(20, true);
            HFONT fVal   = makeFont(19, true);
            HGDIOBJ oldFont = SelectObject(memDC, fRow);

            for (const auto& t : m_texts) {
                HFONT hf = fRow;
                if (t.fontSize >= 26) hf = fTitle;
                else if (t.fontSize <= 16) hf = fSub;
                else if (t.fontSize == 19) hf = fVal;
                else if (t.bold) hf = fRowB;
                SelectObject(memDC, hf);
                SetTextColor(memDC, RGB(t.col.r, t.col.g, t.col.b));
                RECT rc{t.x, t.y, t.x + t.w, t.y + t.h};
                UINT aFlag = DT_LEFT;
                if (t.align == Align::Center) aFlag = DT_CENTER;
                else if (t.align == Align::Right) aFlag = DT_RIGHT;
                DrawTextW(memDC, t.text.c_str(), static_cast<int>(t.text.size()), &rc,
                          DT_SINGLELINE | DT_VCENTER | aFlag);
            }

            SelectObject(memDC, oldFont);
            DeleteObject(fTitle);
            DeleteObject(fSub);
            DeleteObject(fRow);
            DeleteObject(fRowB);
            DeleteObject(fVal);
            SelectObject(memDC, oldBmp);

            const auto* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < totalBytes; i += 4) {
                m_rgba[i + 0] = src[i + 0];
                m_rgba[i + 1] = src[i + 1];
                m_rgba[i + 2] = src[i + 2];
            }
        }
        if (dib) DeleteObject(dib);
        if (memDC) DeleteDC(memDC);
        if (screenDC) ReleaseDC(nullptr, screenDC);

        outPixels.resize(m_rgba.size());
        if (wantBgra) {
            std::memcpy(outPixels.data(), m_rgba.data(), m_rgba.size());
        } else {
            for (size_t i = 0; i < m_rgba.size(); i += 4) {
                outPixels[i + 0] = m_rgba[i + 2];
                outPixels[i + 1] = m_rgba[i + 1];
                outPixels[i + 2] = m_rgba[i + 0];
                outPixels[i + 3] = m_rgba[i + 3];
            }
        }
    }

private:
    struct TextItem {
        int x, y, w, h, fontSize;
        bool bold;
        Align align;
        Color col;
        std::wstring text;
    };

    void SetRawBgra(uint32_t x, uint32_t y, Color c) {
        uint8_t* p = &m_rgba[(static_cast<size_t>(y) * m_w + x) * 4];
        p[0] = c.b;
        p[1] = c.g;
        p[2] = c.r;
        p[3] = c.a;
    }

    void BlendPixel(int x, int y, Color src) {
        if (src.a == 0) return;
        uint8_t* p = &m_rgba[(static_cast<size_t>(y) * m_w + x) * 4];
        const uint8_t sr = src.b;
        const uint8_t sg = src.g;
        const uint8_t sb = src.r;
        if (src.a == 255 || p[3] == 0) {
            p[0] = sr;
            p[1] = sg;
            p[2] = sb;
            p[3] = src.a;
            return;
        }
        const float sa = src.a / 255.0f;
        const float da = p[3] / 255.0f;
        const float oa = sa + da * (1.0f - sa);
        if (oa <= 1e-4f) return;
        p[0] = static_cast<uint8_t>((sr * sa + p[0] * da * (1.0f - sa)) / oa);
        p[1] = static_cast<uint8_t>((sg * sa + p[1] * da * (1.0f - sa)) / oa);
        p[2] = static_cast<uint8_t>((sb * sa + p[2] * da * (1.0f - sa)) / oa);
        p[3] = static_cast<uint8_t>(std::clamp(oa * 255.0f, 0.0f, 255.0f));
    }

    uint32_t m_w;
    uint32_t m_h;
    std::vector<uint8_t> m_rgba;
    std::vector<TextItem> m_texts;
};

struct RowMeta {
    const char* label;
    bool isSlider;
    float minVal;
    float maxVal;
    float step;
    bool isAction;
};

constexpr RowMeta kRows[kRowCount] = {
    {"View Scale",              true,   0.70f,  1.15f, 0.02f, false}, // 0
    {"Image Sharpness",         true,   0.00f,  1.00f, 0.05f, false}, // 1
    {"HUD & Subtitle Lift",     true,   0.00f,  1.00f, 0.05f, false}, // 2
    {"Reticle Size",            true,   0.50f,  2.00f, 0.10f, false}, // 3
    {"World Scale (UU/m)",      true,  30.0f, 100.0f,  2.0f,  false}, // 4
    {"Eye Height Offset",       true, -30.0f,  30.0f,  2.0f,  false}, // 5
    {"Turn Speed",              true,   0.00f,  2.50f, 0.10f, false}, // 6
    {"Move Relative To",        false,  0.0f,   0.0f,  0.0f,  false}, // 7
    {"Match Render Pose",       false,  0.0f,   0.0f,  0.0f,  false}, // 8
    {"Cam-Lock Suppressor",     false,  0.0f,   0.0f,  0.0f,  false}, // 9
    {"Aim From Hand",           false,  0.0f,   0.0f,  0.0f,  false}, // 10
    {"Hide Game Crosshair",     false,  0.0f,   0.0f,  0.0f,  false}, // 11
    {"Recenter View",           false,  0.0f,   0.0f,  0.0f,  true},  // 12
    {"Save Settings",           false,  0.0f,   0.0f,  0.0f,  true},  // 13
    {"Resume Game",             false,  0.0f,   0.0f,  0.0f,  true},  // 14
};

float GetSliderValue(int row) {
    switch (row) {
    case 0: return samu::config::GetViewScale();
    case 1: return samu::config::GetImageSharpness();
    case 2: return samu::config::GetHudTextLift();
    case 3: return samu::config::GetReticleScale();
    case 4: return samu::config::GetWorldScale();
    case 5: return samu::config::GetEyeHeightOffsetCm();
    case 6: return samu::config::GetTurnSpeedScale();
    default: return 0.0f;
    }
}

void SetSliderValue(int row, float v) {
    v = std::clamp(v, kRows[row].minVal, kRows[row].maxVal);
    switch (row) {
    case 0: samu::config::SetViewScale(v); break;
    case 1: samu::config::SetImageSharpness(v); break;
    case 2: samu::config::SetHudTextLift(v); break;
    case 3: samu::config::SetReticleScale(v); break;
    case 4: samu::config::SetWorldScale(v); break;
    case 5: samu::config::SetEyeHeightOffsetCm(v); break;
    case 6: samu::config::SetTurnSpeedScale(v); break;
    default: break;
    }
}

void FormatRowValue(int row, char* out, size_t cap) {
    switch (row) {
    case 0: std::snprintf(out, cap, "%.2f", samu::config::GetViewScale()); break;
    case 1: std::snprintf(out, cap, "%d%%", static_cast<int>(std::round(samu::config::GetImageSharpness() * 100.0f))); break;
    case 2: std::snprintf(out, cap, "%d%%", static_cast<int>(std::round(samu::config::GetHudTextLift() * 100.0f))); break;
    case 3: std::snprintf(out, cap, "%.2fx", samu::config::GetReticleScale()); break;
    case 4: std::snprintf(out, cap, "%.0f", samu::config::GetWorldScale()); break;
    case 5: std::snprintf(out, cap, "%+.0f cm", samu::config::GetEyeHeightOffsetCm()); break;
    case 6: std::snprintf(out, cap, "%.2fx", samu::config::GetTurnSpeedScale()); break;
    case 7: {
        const int m = samu::config::GetLocomotionDirection();
        std::snprintf(out, cap, "%s", m == 0 ? "Body" : (m == 2 ? "Left Hand" : "Head"));
        break;
    }
    case 8: std::snprintf(out, cap, "%s", samu::config::IsMatchRenderPoseEnabled() ? "ON" : "OFF"); break;
    case 9: std::snprintf(out, cap, "%s", samu::config::IsCameraLockSuppressorEnabled() ? "ON" : "OFF"); break;
    case 10: std::snprintf(out, cap, "%s", samu::config::IsAimFromHandEnabled() ? "ON" : "OFF"); break;
    case 11: std::snprintf(out, cap, "%s", samu::config::IsHideGameCrosshairEnabled() ? "ON" : "OFF"); break;
    default: out[0] = '\0'; break;
    }
}

} // namespace

void VrSettingsMenu::SetOpen(bool open) {
    if (m_open == open) return;
    m_open = open;
    m_dragSlider = -1;
    m_prevTrigger = m_prevJump = m_prevMenu = false;
    m_prevNavX = m_prevNavY = 0;
    if (open) {
        SetStatus("Point with Trigger or use Stick + A");
    }
    m_dirty = true;
}

void VrSettingsMenu::SetStatus(const char* msg) {
    m_statusText = msg ? msg : "";
    m_statusUntil = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    m_dirty = true;
}

void VrSettingsMenu::SetSliderFromBarU(int row, float normT) {
    const auto& r = kRows[row];
    if (!r.isSlider) return;
    normT = std::clamp(normT, 0.0f, 1.0f);
    float raw = r.minVal + normT * (r.maxVal - r.minVal);
    float snapped = std::round((raw - r.minVal) / r.step) * r.step + r.minVal;
    if (std::fabs(snapped - GetSliderValue(row)) > 1e-4f) {
        SetSliderValue(row, snapped);
        m_dirty = true;
    }
}

void VrSettingsMenu::AdjustRow(int row, int dir) {
    if (row < 0 || row >= kRowCount || dir == 0) return;
    const auto& r = kRows[row];
    if (r.isSlider) {
        SetSliderValue(row, GetSliderValue(row) + static_cast<float>(dir) * r.step);
        m_dirty = true;
        return;
    }
    switch (row) {
    case 7: samu::config::SetLocomotionDirection((samu::config::GetLocomotionDirection() + dir + 3) % 3); break;
    case 8: samu::config::SetMatchRenderPose(!samu::config::IsMatchRenderPoseEnabled()); break;
    case 9: samu::config::SetCameraLockSuppressor(!samu::config::IsCameraLockSuppressorEnabled()); break;
    case 10: samu::config::SetAimFromHand(!samu::config::IsAimFromHandEnabled()); break;
    case 11: samu::config::SetHideGameCrosshair(!samu::config::IsHideGameCrosshairEnabled()); break;
    default: break;
    }
    m_dirty = true;
}

void VrSettingsMenu::ActivateRow(int row, float clickU) {
    if (row < 0 || row >= kRowCount) return;
    const auto& r = kRows[row];
    if (r.isSlider) {
        const float px = clickU * kWidth;
        if (px >= kBarX0 - 12 && px <= kBarX1 + 12) {
            m_dragSlider = row;
            SetSliderFromBarU(row, (px - kBarX0) / static_cast<float>(kBarX1 - kBarX0));
        } else {
            AdjustRow(row, (px < kBarX0) ? -1 : +1);
        }
        return;
    }
    if (!r.isAction) {
        AdjustRow(row, +1);
        return;
    }
    if (row == 12) {
        SetStatus("View recentered to current pose");
    } else if (row == 13) {
        const bool saved = samu::config::SaveSettings();
        SetStatus(saved ? "Settings saved to samu_vr.ini" : "Save failed (check folder permissions)");
    }
}

VrMenuUpdateResult VrSettingsMenu::Update(const ControllerInputState& input,
                                          bool hasRayHit, float rayU, float rayV) {
    VrMenuUpdateResult res;
    if (!m_open) return res;

    const auto now = std::chrono::steady_clock::now();
    if (!m_statusText.empty() && now >= m_statusUntil) {
        m_statusText.clear();
        m_dirty = true;
    }

    if (hasRayHit) {
        const float du = std::fabs(rayU - m_cursorU);
        const float dv = std::fabs(rayV - m_cursorV);
        if (!m_hasCursor || du > 0.002f || dv > 0.002f) {
            m_hasCursor = true;
            m_cursorU = rayU;
            m_cursorV = rayV;
            m_dirty = true;
        }
        const int py = static_cast<int>(rayV * kHeight);
        if (py >= kRowStartY && py < kRowStartY + kRowCount * kRowH) {
            const int hoverRow = (py - kRowStartY) / kRowH;
            if (hoverRow != m_selectedRow && m_dragSlider < 0) {
                m_selectedRow = hoverRow;
                m_dirty = true;
            }
        }
    } else if (m_hasCursor) {
        m_hasCursor = false;
        m_dirty = true;
    }

    const bool trigger = input.active && (input.rightTriggerPressed || input.leftTriggerPressed);
    const bool jump    = input.active && input.jumpButtonPressed;
    const bool menu    = input.active && input.menuButtonPressed;

    if (trigger && hasRayHit) {
        if (!m_prevTrigger) {
            if (m_selectedRow == 12) res.recenterRequested = true;
            if (m_selectedRow == 14) res.closeRequested    = true;
            ActivateRow(m_selectedRow, rayU);
        } else if (m_dragSlider >= 0) {
            const float px = rayU * kWidth;
            SetSliderFromBarU(m_dragSlider, (px - kBarX0) / static_cast<float>(kBarX1 - kBarX0));
        }
    } else if (!trigger) {
        m_dragSlider = -1;
    }

    float sx = (std::fabs(input.rightStick.x) > std::fabs(input.leftStick.x)) ? input.rightStick.x : input.leftStick.x;
    float sy = (std::fabs(input.rightStick.y) > std::fabs(input.leftStick.y)) ? input.rightStick.y : input.leftStick.y;
    const int navY = (sy > 0.55f) ? -1 : ((sy < -0.55f) ? +1 : 0);
    const int navX = (sx > 0.55f) ? +1 : ((sx < -0.55f) ? -1 : 0);

    if (navY != 0 && (navY != m_prevNavY || now >= m_nextRepeatY)) {
        m_selectedRow = (m_selectedRow + navY + kRowCount) % kRowCount;
        m_dirty = true;
        m_nextRepeatY = now + std::chrono::milliseconds(navY != m_prevNavY ? 280 : 110);
    }
    if (navX != 0 && (navX != m_prevNavX || now >= m_nextRepeatX)) {
        AdjustRow(m_selectedRow, navX);
        m_nextRepeatX = now + std::chrono::milliseconds(navX != m_prevNavX ? 280 : 95);
    }
    m_prevNavY = navY;
    m_prevNavX = navX;

    if (jump && !m_prevJump) {
        if (m_selectedRow == 12) res.recenterRequested = true;
        if (m_selectedRow == 14) res.closeRequested    = true;
        ActivateRow(m_selectedRow, 0.85f);
    }

    if (menu && !m_prevMenu) {
        res.closeRequested = true;
    }

    m_prevTrigger = trigger;
    m_prevJump    = jump;
    m_prevMenu    = menu;
    return res;
}

bool VrSettingsMenu::RenderIfDirty(bool bgra, std::vector<uint8_t>& outPixels) {
    if (!m_dirty) return false;
    m_dirty = false;

    Canvas cv(kWidth, kHeight);
    cv.FillPanelBackdrop(10.0f);

    // Clean SAMU-style minimal header
    cv.QueueText(kPadX, 18, static_cast<int>(kWidth) - 2 * kPadX, 34, 28, true, Canvas::Align::Center, kColTextHi, "VR SETTINGS");
    cv.QueueText(kPadX, 54, static_cast<int>(kWidth) - 2 * kPadX, 24, 16, false, Canvas::Align::Center, kColTextDim, "Click Right Stick or F10 to close");
    cv.FillRect(kPadX, kHeaderH, static_cast<int>(kWidth) - 2 * kPadX, 1, kColRule);

    char valBuf[48]{};
    for (int i = 0; i < kRowCount; ++i) {
        const auto& r = kRows[i];
        const int ry = kRowStartY + i * kRowH;
        const bool sel = (i == m_selectedRow);

        if (i == 12) {
            // Subtle divider separating settings from bottom actions
            cv.FillRect(kPadX, ry - 4, static_cast<int>(kWidth) - 2 * kPadX, 1, kColRule);
        }

        if (r.isAction) {
            cv.FillRoundedRect(static_cast<float>(kPadX), static_cast<float>(ry + 5),
                               static_cast<float>(kWidth - 2 * kPadX), static_cast<float>(kRowH - 10),
                               4.0f, sel ? kColBtnHover : kColBtnIdle, 1.0f, sel ? kColAccent : kColBtnEdge);
            cv.QueueText(kPadX, ry + 5, static_cast<int>(kWidth) - 2 * kPadX, kRowH - 10, 20, sel, Canvas::Align::Center,
                         sel ? kColTextHi : kColTextMid, r.label);
            continue;
        }

        if (sel) {
            cv.FillRoundedRect(static_cast<float>(kPadX), static_cast<float>(ry + 3),
                               static_cast<float>(kWidth - 2 * kPadX), static_cast<float>(kRowH - 6),
                               4.0f, kColRowHover);
            cv.FillRect(kPadX, ry + 8, 3, kRowH - 16, kColAccent);
        }

        cv.QueueText(kPadX + 18, ry, 340, kRowH, 20, sel, Canvas::Align::Left, sel ? kColTextHi : kColTextMid, r.label);
        FormatRowValue(i, valBuf, sizeof(valBuf));

        if (r.isSlider) {
            const float barY = static_cast<float>(ry + kRowH / 2 - 2);
            const float barH = 4.0f;
            const float barW = static_cast<float>(kBarX1 - kBarX0);
            cv.FillRoundedRect(static_cast<float>(kBarX0), barY, barW, barH, 2.0f, kColBarTrack);
            const float t = std::clamp((GetSliderValue(i) - r.minVal) / (r.maxVal - r.minVal), 0.0f, 1.0f);
            const float fillW = std::max(barH, t * barW);
            cv.FillRoundedRect(static_cast<float>(kBarX0), barY, fillW, barH, 2.0f, sel ? kColAccent : kColBarIdle);
            cv.FillCircle(static_cast<float>(kBarX0) + t * barW, barY + barH * 0.5f, sel ? 8.0f : 6.5f,
                          sel ? kColTextHi : kColBarIdle);
            cv.QueueText(kBarX1 + 14, ry, static_cast<int>(kWidth) - kPadX - (kBarX1 + 14) - 10, kRowH, 19, true,
                         Canvas::Align::Right, sel ? kColAccent : kColTextHi, valBuf);
        } else {
            const bool isOn = (std::strcmp(valBuf, "ON") == 0);
            const bool isOff = (std::strcmp(valBuf, "OFF") == 0);
            const Color vc = isOn ? kColToggleOn : (isOff ? kColToggleOff : (sel ? kColAccent : kColTextHi));
            cv.QueueText(kBarX0, ry, static_cast<int>(kWidth) - kPadX - kBarX0 - 10, kRowH, 19, true,
                         Canvas::Align::Right, vc, valBuf);
        }
    }

    const int footerY = kRowStartY + kRowCount * kRowH + 14;
    cv.FillRect(kPadX, footerY, static_cast<int>(kWidth) - 2 * kPadX, 1, kColRule);
    const char* footer = !m_statusText.empty() ? m_statusText.c_str()
                                               : "Changes apply immediately. Use Save Settings to keep them.";
    cv.QueueText(kPadX, footerY + 12, static_cast<int>(kWidth) - 2 * kPadX, 38, 16, false, Canvas::Align::Center,
                 !m_statusText.empty() ? kColAccent : kColTextDim, footer);

    if (m_hasCursor) {
        const float cx = m_cursorU * kWidth;
        const float cy = m_cursorV * kHeight;
        cv.FillCircle(cx, cy, 8.0f, Color{232, 186, 108, 120});
        cv.FillCircle(cx, cy, 4.0f, Color{255, 255, 255, 255});
    }

    cv.FinalizeTo(bgra, outPixels);
    return true;
}

} // namespace vr
