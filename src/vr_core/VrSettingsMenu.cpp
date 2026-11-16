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

constexpr int kRowCount   = 16;
constexpr int kHeaderH    = 78;
constexpr int kRowStartY  = 86;
constexpr int kRowH       = 43;
constexpr int kPadX       = 26;
constexpr int kBarX0      = 360;
constexpr int kBarX1      = 618;

struct Color {
    uint8_t r, g, b, a;
};

// A Story About My Uncle cavern-slate & luminous glove-cyan palette (subtle, modern, warm ivory typography).
constexpr Color kColBgTop      {10,  18,  29, 245};
constexpr Color kColBgBot      { 7,  13,  21, 246};
constexpr Color kColBgHeader   {13,  25,  40, 248};
constexpr Color kColBgFooter   { 9,  16,  26, 246};
constexpr Color kColRowHover   {24,  46,  72, 242};
constexpr Color kColRowIdle    {15,  26,  40, 215};
constexpr Color kColBtnIdle    {20,  37,  58, 232};
constexpr Color kColBtnHover   {31,  60,  92, 246};
constexpr Color kColBorder     {44,  78, 108, 225};
constexpr Color kColAccent     {108, 214, 245, 255};   // Grapple icy sky-cyan
constexpr Color kColAccentSoft { 66, 166, 206, 235};
constexpr Color kColWarmGold   {244, 198, 114, 255};   // Crystal/cave-torch warm highlight (ON/saved)
constexpr Color kColTextHi     {238, 244, 250, 255};   // Warm chalk-white
constexpr Color kColTextMid    {166, 190, 212, 255};   // Muted mist-slate
constexpr Color kColTextDim    {124, 152, 178, 255};
constexpr Color kColBarTrack   { 24,  38,  56, 255};
constexpr Color kColBarFill    { 72, 176, 216, 255};

float DistToSegment(float px, float py, float x1, float y1, float x2, float y2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float lenSq = dx * dx + dy * dy;
    const float t = (lenSq > 1e-5f) ? std::clamp(((px - x1) * dx + (py - y1) * dy) / lenSq, 0.0f, 1.0f) : 0.0f;
    const float qx = x1 + dx * t - px;
    const float qy = y1 + dy * t - py;
    return std::sqrt(qx * qx + qy * qy);
}

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

    void FillPanelGradient(float cornerRad) {
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
                uint8_t r = br, g = bg, b = bb;
                // Subtle luminous blue ambient wash behind top header
                const float dx = (px + 0.5f) - 180.0f;
                const float dy = (py + 0.5f) - 42.0f;
                const float gDist = std::sqrt(dx * dx + dy * dy);
                if (gDist < 240.0f) {
                    const float glow = (1.0f - gDist / 240.0f);
                    const float wgt = glow * glow * 0.45f;
                    r = static_cast<uint8_t>(std::min(255.0f, r + 18.0f * wgt));
                    g = static_cast<uint8_t>(std::min(255.0f, g + 56.0f * wgt));
                    b = static_cast<uint8_t>(std::min(255.0f, b + 82.0f * wgt));
                }
                // Crisp outer slate-cyan rim
                if (sd > -2.2f && sd <= 0.5f) {
                    const float rim = std::clamp(1.0f - std::fabs(sd + 1.0f) / 1.4f, 0.0f, 1.0f);
                    r = static_cast<uint8_t>(r + (kColBorder.r - r) * rim);
                    g = static_cast<uint8_t>(g + (kColBorder.g - g) * rim);
                    b = static_cast<uint8_t>(b + (kColBorder.b - b) * rim);
                }
                SetRawBgra(px, py, Color{r, g, b, static_cast<uint8_t>(ba * cov)});
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

    void FillRoundedRect(float x, float y, float w, float h, float rad, Color fill,
                         float borderW = 0.0f, Color border = Color{0, 0, 0, 0}) {
        const int x0 = std::max(0, static_cast<int>(std::floor(x - 1.0f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(y - 1.0f)));
        const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(x + w + 1.0f)));
        const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(y + h + 1.0f)));
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float sd = RoundedBoxSdf(px + 0.5f, py + 0.5f, x, y, w, h, rad);
                const float fillCov = std::clamp(0.5f - sd, 0.0f, 1.0f);
                if (fillCov > 0.0f && fill.a > 0) {
                    Color c = fill;
                    c.a = static_cast<uint8_t>(fill.a * fillCov);
                    BlendPixel(px, py, c);
                }
                if (borderW > 0.0f && border.a > 0 && sd > -(borderW + 1.0f) && sd < 1.0f) {
                    const float inner = sd + borderW;
                    const float edgeCov = std::clamp(0.5f - sd, 0.0f, 1.0f) * std::clamp(inner + 0.5f, 0.0f, 1.0f);
                    if (edgeCov > 0.0f) {
                        Color bc = border;
                        bc.a = static_cast<uint8_t>(border.a * edgeCov);
                        BlendPixel(px, py, bc);
                    }
                }
            }
        }
    }

    void FillDiamond(float cx, float cy, float r, Color c) {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 2.0f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 2.0f)));
        const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(cx + r + 2.0f)));
        const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(cy + r + 2.0f)));
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float d = (std::fabs(px + 0.5f - cx) + std::fabs(py + 0.5f - cy)) * 0.70710678f;
                const float cov = std::clamp(r * 0.70710678f + 0.5f - d, 0.0f, 1.0f);
                if (cov > 0.0f) {
                    Color sc = c;
                    sc.a = static_cast<uint8_t>(c.a * cov);
                    BlendPixel(px, py, sc);
                }
            }
        }
    }

    // Draws ASAMU's signature open-right diamond crosshair icon cleanly with glow.
    void DrawSamuCrosshairEmblem(float cx, float cy, float r) {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 8.0f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 8.0f)));
        const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(cx + r + 8.0f)));
        const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(cy + r + 8.0f)));
        const float topX = cx, topY = cy - r * 0.85f;
        const float botX = cx, botY = cy + r * 0.85f;
        const float lftX = cx - r * 0.90f, lftY = cy;
        const float trX  = cx + r * 0.55f, trY  = cy - r * 0.34f;
        const float brX  = cx + r * 0.55f, brY  = cy + r * 0.34f;
        const float tTipY = cy - r * 1.18f;
        const float bTipY = cy + r * 1.18f;
        const float rHookX = cx + r * 0.78f;
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float fx = px + 0.5f;
                const float fy = py + 0.5f;
                float d = DistToSegment(fx, fy, topX, topY, lftX, lftY);
                d = std::min(d, DistToSegment(fx, fy, lftX, lftY, botX, botY));
                d = std::min(d, DistToSegment(fx, fy, topX, topY, trX, trY));
                d = std::min(d, DistToSegment(fx, fy, botX, botY, brX, brY));
                d = std::min(d, DistToSegment(fx, fy, trX, trY, rHookX, cy - r * 0.52f));
                d = std::min(d, DistToSegment(fx, fy, brX, brY, rHookX, cy + r * 0.52f));
                d = std::min(d, DistToSegment(fx, fy, topX, tTipY, topX, topY));
                d = std::min(d, DistToSegment(fx, fy, botX, botY, botX, bTipY));
                const float dotD = std::sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) - r * 0.14f;
                d = std::min(d, std::max(0.0f, dotD));

                const float core = std::clamp(1.75f - d, 0.0f, 1.0f);
                const float halo = std::clamp(1.0f - d / 5.5f, 0.0f, 1.0f);
                if (core > 0.0f) {
                    BlendPixel(px, py, Color{225, 250, 255, static_cast<uint8_t>(245 * core)});
                } else if (halo > 0.0f) {
                    BlendPixel(px, py, Color{80, 200, 240, static_cast<uint8_t>(95 * halo * halo)});
                }
            }
        }
    }

    void FillCircle(float cx, float cy, float r, Color c) {
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.0f)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.0f)));
        const int x1 = std::min(static_cast<int>(m_w), static_cast<int>(std::ceil(cx + r + 1.0f)));
        const int y1 = std::min(static_cast<int>(m_h), static_cast<int>(std::ceil(cy + r + 1.0f)));
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

    struct TextItem {
        int x, y, w, h;
        int fontSize;
        bool bold;
        bool centerH;
        Color col;
        std::wstring text;
    };

    void QueueText(int x, int y, int w, int h, int fontSize, bool bold, bool centerH, Color c, const char* txt) {
        if (!txt || !*txt) return;
        std::wstring wtxt;
        for (const char* p = txt; *p; ++p) wtxt.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
        m_texts.push_back(TextItem{x, y, w, h, fontSize, bold, centerH, c, std::move(wtxt)});
    }

    void FinalizeTo(bool wantBgra, std::vector<uint8_t>& outPixels) {
        // Rasterize queued anti-aliased Segoe UI text onto a top-down 32bpp DIB initialized from m_rgba (stored BGRA),
        // preserving our rounded outer alpha mask so crisp GDI typography keeps proper panel transparency.
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = static_cast<LONG>(m_w);
        bmi.bmiHeader.biHeight      = -static_cast<LONG>(m_h); // top-down
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
            HFONT fTitle = makeFont(24, true);
            HFONT fSub   = makeFont(14, false);
            HFONT fRow   = makeFont(17, false);
            HFONT fVal   = makeFont(16, true);
            HFONT fBtn   = makeFont(17, true);
            HGDIOBJ oldFont = SelectObject(memDC, fRow);

            for (const auto& t : m_texts) {
                HFONT hf = fRow;
                if (t.fontSize >= 22) hf = fTitle;
                else if (t.fontSize <= 14) hf = fSub;
                else if (t.fontSize == 17 && t.bold) hf = fBtn;
                else if (t.bold) hf = fVal;
                SelectObject(memDC, hf);
                SetTextColor(memDC, RGB(t.col.r, t.col.g, t.col.b));
                RECT rc{t.x, t.y, t.x + t.w, t.y + t.h};
                const UINT fmt = DT_SINGLELINE | DT_VCENTER | (t.centerH ? DT_CENTER : DT_LEFT);
                DrawTextW(memDC, t.text.c_str(), static_cast<int>(t.text.size()), &rc, fmt);
            }

            SelectObject(memDC, oldFont);
            DeleteObject(fTitle);
            DeleteObject(fSub);
            DeleteObject(fRow);
            DeleteObject(fVal);
            DeleteObject(fBtn);
            SelectObject(memDC, oldBmp);

            // Copy RGB from DIB while preserving m_rgba's exact alpha channel!
            const auto* src = static_cast<const uint8_t*>(bits);
            for (size_t i = 0; i < totalBytes; i += 4) {
                m_rgba[i + 0] = src[i + 0];
                m_rgba[i + 1] = src[i + 1];
                m_rgba[i + 2] = src[i + 2];
                // keep m_rgba[i + 3] alpha untouched (GDI clears alpha byte on glyphs)
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
        const uint8_t sr = src.b; // internal buffer is BGRA (matching Windows 32bpp DIB)
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
    std::vector<uint8_t> m_rgba; // BGRA pixel storage
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
    {"View Scale (1=Full)",    true,   0.70f,  1.15f, 0.02f, false}, // 0
    {"Image Sharpness",        true,   0.00f,  1.00f, 0.05f, false}, // 1
    {"Text / HUD Lift",        true,   0.00f,  1.00f, 0.05f, false}, // 2
    {"Crosshair Size",         true,   0.50f,  2.00f, 0.10f, false}, // 3
    {"World Scale (UU/m)",     true,  30.0f, 100.0f,  2.0f,  false}, // 4
    {"Eye Height Offset",      true, -30.0f,  30.0f,  2.0f,  false}, // 5
    {"Turn Speed",             true,   0.00f,  2.50f, 0.10f, false}, // 6
    {"Move Relative To",       false,  0.0f,   0.0f,  0.0f,  false}, // 7
    {"Stereo Mode",            false,  0.0f,   0.0f,  0.0f,  false}, // 8
    {"Match Render Pose",      false,  0.0f,   0.0f,  0.0f,  false}, // 9
    {"Cam-Lock Suppressor",    false,  0.0f,   0.0f,  0.0f,  false}, // 10
    {"Aim From Hand",          false,  0.0f,   0.0f,  0.0f,  false}, // 11
    {"Hide Game Crosshair",    false,  0.0f,   0.0f,  0.0f,  false}, // 12
    {"[ Recenter View (HMD & Menu) ]", false, 0, 0, 0, true},       // 13
    {"[ Save Settings to samu_vr.ini ]", false, 0, 0, 0, true},     // 14
    {"[ Close VR Settings Menu ]",       false, 0, 0, 0, true},     // 15
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
    case 8: std::snprintf(out, cap, "%s", samu::config::GetStereoMode() == 1 ? "AFR (3D)" : "MONO (Smooth)"); break;
    case 9: std::snprintf(out, cap, "%s", samu::config::IsMatchRenderPoseEnabled() ? "ON" : "OFF"); break;
    case 10: std::snprintf(out, cap, "%s", samu::config::IsCameraLockSuppressorEnabled() ? "ON" : "OFF"); break;
    case 11: std::snprintf(out, cap, "%s", samu::config::IsAimFromHandEnabled() ? "ON" : "OFF"); break;
    case 12: std::snprintf(out, cap, "%s", samu::config::IsHideGameCrosshairEnabled() ? "ON" : "OFF"); break;
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
        SetStatus("Point + Trigger or Thumbstick + A to adjust");
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
    // Snap cleanly to step increments.
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
    case 8: samu::config::SetStereoMode(samu::config::GetStereoMode() == 1 ? 0 : 1); break;
    case 9: samu::config::SetMatchRenderPose(!samu::config::IsMatchRenderPoseEnabled()); break;
    case 10: samu::config::SetCameraLockSuppressor(!samu::config::IsCameraLockSuppressorEnabled()); break;
    case 11: samu::config::SetAimFromHand(!samu::config::IsAimFromHandEnabled()); break;
    case 12: samu::config::SetHideGameCrosshair(!samu::config::IsHideGameCrosshairEnabled()); break;
    default: break;
    }
    m_dirty = true;
}

void VrSettingsMenu::ActivateRow(int row, float clickU) {
    if (row < 0 || row >= kRowCount) return;
    const auto& r = kRows[row];
    if (r.isSlider) {
        const float px = clickU * kWidth;
        if (px >= kBarX0 - 10 && px <= kBarX1 + 10) {
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
    if (row == 13) {
        SetStatus("View recentered to current pose!");
    } else if (row == 14) {
        const bool saved = samu::config::SaveSettings();
        SetStatus(saved ? "Saved all settings to samu_vr.ini!" : "Save failed (check folder permissions)");
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

    // Trigger click / slider drag via raycast.
    if (trigger && hasRayHit) {
        if (!m_prevTrigger) {
            if (m_selectedRow == 13) res.recenterRequested = true;
            if (m_selectedRow == 15) res.closeRequested    = true;
            ActivateRow(m_selectedRow, rayU);
        } else if (m_dragSlider >= 0) {
            const float px = rayU * kWidth;
            SetSliderFromBarU(m_dragSlider, (px - kBarX0) / static_cast<float>(kBarX1 - kBarX0));
        }
    } else if (!trigger) {
        m_dragSlider = -1;
    }

    // Thumbstick navigation (either stick works).
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

    // A / X activates current row.
    if (jump && !m_prevJump) {
        if (m_selectedRow == 13) res.recenterRequested = true;
        if (m_selectedRow == 15) res.closeRequested    = true;
        ActivateRow(m_selectedRow, 0.85f);
    }

    // B / Left Menu closes menu cleanly.
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
    cv.FillPanelGradient(14.0f);

    // Header band + SAMU grapple diamond emblem + subtle hairline divider
    cv.FillRect(14, 2, static_cast<int>(kWidth) - 28, kHeaderH - 4, kColBgHeader);
    cv.DrawSamuCrosshairEmblem(182.0f, 38.0f, 19.0f);
    cv.FillRect(kPadX, kHeaderH - 2, static_cast<int>(kWidth) - 2 * kPadX, 1, Color{72, 168, 212, 145});

    cv.QueueText(218, 12, 420, 28, 24, true, false, kColTextHi, "VR SETTINGS");
    cv.QueueText(218, 42, 460, 22, 14, false, false, kColTextDim, "R3 / F10: Close   |   Trigger or Stick + A: Adjust");

    // Footer separator band
    const int footerY = kRowStartY + kRowCount * kRowH + 8;
    cv.FillRect(14, footerY, static_cast<int>(kWidth) - 28, static_cast<int>(kHeight) - footerY - 12, kColBgFooter);
    cv.FillRect(kPadX, footerY, static_cast<int>(kWidth) - 2 * kPadX, 1, Color{44, 78, 108, 155});

    char valBuf[48]{};
    for (int i = 0; i < kRowCount; ++i) {
        const auto& r = kRows[i];
        const int ry = kRowStartY + i * kRowH;
        const bool sel = (i == m_selectedRow);

        if (r.isAction) {
            const Color btnBorder = sel ? kColAccent : kColBorder;
            cv.FillRoundedRect(static_cast<float>(kPadX), static_cast<float>(ry + 4),
                               static_cast<float>(kWidth - 2 * kPadX), static_cast<float>(kRowH - 8),
                               7.0f, sel ? kColBtnHover : kColBtnIdle, sel ? 1.5f : 1.0f, btnBorder);
            if (sel) cv.FillDiamond(static_cast<float>(kPadX + 20), static_cast<float>(ry + kRowH / 2), 6.0f, kColAccent);
            cv.QueueText(kPadX, ry + 4, static_cast<int>(kWidth) - 2 * kPadX, kRowH - 8, 17, true, true,
                         sel ? kColTextHi : kColAccent, r.label);
            continue;
        }

        cv.FillRoundedRect(static_cast<float>(kPadX), static_cast<float>(ry + 3),
                           static_cast<float>(kWidth - 2 * kPadX), static_cast<float>(kRowH - 6),
                           6.0f, sel ? kColRowHover : kColRowIdle, sel ? 1.5f : 0.75f,
                           sel ? kColAccentSoft : Color{32, 54, 78, 110});
        if (sel) {
            cv.FillRect(kPadX + 2, ry + 8, 3, kRowH - 16, kColAccent);
        }

        cv.QueueText(kPadX + 16, ry + 3, 300, kRowH - 6, 17, sel, false, sel ? kColTextHi : kColTextMid, r.label);
        FormatRowValue(i, valBuf, sizeof(valBuf));

        if (r.isSlider) {
            const float barY = static_cast<float>(ry + 18);
            const float barH = 7.0f;
            const float barW = static_cast<float>(kBarX1 - kBarX0);
            cv.FillRoundedRect(static_cast<float>(kBarX0), barY, barW, barH, 3.5f, kColBarTrack);
            const float t = std::clamp((GetSliderValue(i) - r.minVal) / (r.maxVal - r.minVal), 0.0f, 1.0f);
            const float fillW = std::max(barH, t * barW);
            cv.FillRoundedRect(static_cast<float>(kBarX0), barY, fillW, barH, 3.5f, sel ? kColAccent : kColBarFill);
            cv.FillDiamond(static_cast<float>(kBarX0) + t * barW, barY + barH * 0.5f, 8.5f,
                           sel ? kColTextHi : Color{215, 238, 250, 255});
            cv.QueueText(kBarX1 + 18, ry + 3, 110, kRowH - 6, 16, true, false,
                         sel ? kColAccent : kColTextHi, valBuf);
        } else {
            const int pillW = 148;
            const int pillX = static_cast<int>(kWidth) - kPadX - 14 - pillW;
            const bool isOn = (std::strcmp(valBuf, "ON") == 0);
            const Color pillFill = isOn ? Color{28, 62, 84, 240} : (sel ? kColBtnHover : kColBtnIdle);
            const Color pillEdge = isOn ? Color{88, 186, 224, 210} : (sel ? kColAccent : kColBorder);
            cv.FillRoundedRect(static_cast<float>(pillX), static_cast<float>(ry + 7),
                               static_cast<float>(pillW), static_cast<float>(kRowH - 14),
                               6.0f, pillFill, 1.0f, pillEdge);
            const Color vc = isOn ? kColWarmGold : (sel ? kColTextHi : kColAccent);
            cv.QueueText(pillX, ry + 7, pillW, kRowH - 14, 16, true, true, vc, valBuf);
        }
    }

    // Footer status message
    const char* footer = !m_statusText.empty() ? m_statusText.c_str()
                                               : "Changes apply immediately. Press Save Settings to persist.";
    cv.QueueText(kPadX, footerY + 6, static_cast<int>(kWidth) - 2 * kPadX, 42, 14, !m_statusText.empty(), true,
                 !m_statusText.empty() ? kColWarmGold : kColTextDim, footer);

    // Diamond-accented laser pointer cursor (drawn behind GDI? Fine here BEFORE finalize so it never smears text)
    if (m_hasCursor) {
        const float cx = m_cursorU * kWidth;
        const float cy = m_cursorV * kHeight;
        cv.FillCircle(cx, cy, 9.0f, Color{96, 214, 248, 130});
        cv.FillDiamond(cx, cy, 5.0f, Color{240, 250, 255, 255});
    }

    cv.FinalizeTo(bgra, outPixels);
    return true;
}

} // namespace vr
