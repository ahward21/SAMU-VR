#include "vr_core/OpenXRManager.h"
#include "vr_core/DX9DX11Bridge.h"
#include "vr_core/XrMath.h"
#include "common/Config.h"
#include "common/Log.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

// XR_USE_PLATFORM_WIN32 / XR_USE_GRAPHICS_API_D3D11 are defined by CMake (vr_core target).
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vr {
namespace {

template <typename T> void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

constexpr uint32_t kDefaultClampedEyeExtent = 1440u;

void ClampedEyeExtent(const XrViewConfigurationView& view, uint32_t& outW, uint32_t& outH) {
    const auto& cfg = samu::config::Get();
    const uint32_t recW = view.recommendedImageRectWidth  ? view.recommendedImageRectWidth  : kDefaultClampedEyeExtent;
    const uint32_t recH = view.recommendedImageRectHeight ? view.recommendedImageRectHeight : kDefaultClampedEyeExtent;
    // Keep OpenXR swapchains sharp (>=1440 px) rather than shrinking them to the windowed desktop height.
    uint32_t maxH = kDefaultClampedEyeExtent;
    if (cfg.renderHeight > 0) {
        maxH = std::max(kDefaultClampedEyeExtent, static_cast<uint32_t>(cfg.renderHeight));
    }
    outW = std::max(480u, std::min(recW, maxH) & ~7u);
    outH = std::max(480u, std::min(recH, maxH) & ~7u);
}

struct BindingSet {
    const char* profile;
    bool hasAim;
};

// Every listed profile exposes grip/pose and aim/pose on both hands.
constexpr BindingSet kProfiles[] = {
    {"/interaction_profiles/khr/simple_controller", true},
    {"/interaction_profiles/oculus/touch_controller", true},
    {"/interaction_profiles/valve/index_controller", true},
    {"/interaction_profiles/htc/vive_controller", true},
    {"/interaction_profiles/microsoft/motion_controller", true},
};

} // namespace

OpenXRManager::~OpenXRManager() { Shutdown(); }

bool OpenXRManager::Check(XrResult r, const char* what) const {
    if (XR_SUCCEEDED(r)) return true;
    char buf[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (m_instance != XR_NULL_HANDLE) {
        xrResultToString(m_instance, r, buf);
    } else {
        switch (r) {
        case XR_ERROR_RUNTIME_FAILURE: std::strcpy(buf, "XR_ERROR_RUNTIME_FAILURE"); break;
        case XR_ERROR_RUNTIME_UNAVAILABLE: std::strcpy(buf, "XR_ERROR_RUNTIME_UNAVAILABLE"); break;
        case XR_ERROR_INSTANCE_LOST: std::strcpy(buf, "XR_ERROR_INSTANCE_LOST"); break;
        case XR_ERROR_EXTENSION_NOT_PRESENT: std::strcpy(buf, "XR_ERROR_EXTENSION_NOT_PRESENT"); break;
        case XR_ERROR_API_VERSION_UNSUPPORTED: std::strcpy(buf, "XR_ERROR_API_VERSION_UNSUPPORTED"); break;
        default: break;
        }
    }
    LOG_ERROR("OpenXR: %s failed: %s (%d)", what, buf, static_cast<int>(r));
    return false;
}

const char* OpenXRManager::SessionStateName(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

bool OpenXRManager::Initialize() {
    if (IsInitialized()) return true;
    LOG_INFO("OpenXR: initializing (Phase 3 stereo swapchain + DX9-to-DX11 bridge)...");
    if (CreateInstance() && CreateD3D11Device() && CreateSession() &&
        CreateSwapchains() && CreateSpaces() && CreateActions()) {
        CreateReticleSwapchain(m_reticleSwapchain, false);   // optional: failure only disables the hand reticle
        CreateReticleSwapchain(m_reticleLitSwapchain, true);
        CreateVrMenuSwapchain();
        DX9DX11Bridge::BindOpenXRManager(this);
        LOG_INFO("OpenXR: initialization complete, waiting for session READY.");
        return true;
    }
    Shutdown();
    return false;
}

bool OpenXRManager::CreateInstance() {
    uint32_t extCount = 0;
    if (!Check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr),
               "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasD3D11 = false;
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0) hasD3D11 = true;
    LOG_INFO("OpenXR: runtime exposes %u extensions; XR_KHR_D3D11_enable=%s", extCount,
             hasD3D11 ? "yes" : "NO");
    if (!hasD3D11) return false;

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    std::strncpy(ci.applicationInfo.applicationName, "SAMU-VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    std::strncpy(ci.applicationInfo.engineName, "UnrealEngine3", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.engineVersion = 3;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0; // widest runtime compatibility
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled;
    if (!Check(xrCreateInstance(&ci, &m_instance), "xrCreateInstance")) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(m_instance, &ip);
    LOG_INFO("OpenXR: runtime '%s' v%u.%u.%u", ip.runtimeName,
             XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
             XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(m_instance, &sgi, &m_systemId), "xrGetSystem (HMD)")) return false;

    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(m_instance, m_systemId, &sp);
    LOG_INFO("OpenXR: system '%s' vendor=0x%X maxRes=%ux%u posTracking=%d oriTracking=%d",
             sp.systemName, sp.vendorId, sp.graphicsProperties.maxSwapchainImageWidth,
             sp.graphicsProperties.maxSwapchainImageHeight,
             sp.trackingProperties.positionTracking, sp.trackingProperties.orientationTracking);

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      viewCount, &viewCount, views.data());
    for (uint32_t i = 0; i < viewCount && i < 2; ++i) {
        m_viewConfigs[i] = views[i];
        LOG_INFO("OpenXR: view[%u] recommended %ux%u (samples %u)", i,
                 views[i].recommendedImageRectWidth, views[i].recommendedImageRectHeight,
                 views[i].recommendedSwapchainSampleCount);
    }
    return viewCount >= 2;
}

bool OpenXRManager::CreateD3D11Device() {
    PFN_xrGetD3D11GraphicsRequirementsKHR getReqs = nullptr;
    if (!Check(xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&getReqs)),
               "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)"))
        return false;

    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    if (!Check(getReqs(m_instance, m_systemId, &reqs), "xrGetD3D11GraphicsRequirementsKHR")) return false;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        LOG_ERROR("CreateDXGIFactory1 failed");
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        if (std::memcmp(&d.AdapterLuid, &reqs.adapterLuid, sizeof(LUID)) == 0) {
            LOG_INFO("D3D11: using adapter '%ls' (%zu MB VRAM)", d.Description,
                     static_cast<size_t>(d.DedicatedVideoMemory >> 20));
            break;
        }
        SafeRelease(adapter);
    }
    SafeRelease(factory);
    if (!adapter) {
        LOG_ERROR("D3D11: no DXGI adapter matches the OpenXR runtime LUID");
        return false;
    }

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &m_d3d11Device, &got,
                                   &m_d3d11Context);
    SafeRelease(adapter);
    if (FAILED(hr)) {
        LOG_ERROR("D3D11CreateDevice failed hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    if (got < reqs.minFeatureLevel) {
        LOG_ERROR("D3D11: feature level 0x%X below runtime minimum 0x%X", got, reqs.minFeatureLevel);
        return false;
    }

    ID3D11Multithread* mt = nullptr;
    if ((m_d3d11Context && SUCCEEDED(m_d3d11Context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt)))) ||
        (m_d3d11Device  && SUCCEEDED(m_d3d11Device->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&mt))))) {
        if (mt) {
            mt->SetMultithreadProtected(TRUE);
            SafeRelease(mt);
            LOG_INFO("D3D11: enabled ID3D11Multithread protection on device/context.");
        }
    }

    LOG_INFO("D3D11: device created (feature level 0x%X)", got);
    return true;
}

bool OpenXRManager::CreateSession() {
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = m_d3d11Device;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = m_systemId;
    if (!Check(xrCreateSession(m_instance, &ci, &m_session), "xrCreateSession")) return false;
    LOG_INFO("OpenXR: session created.");
    return true;
}

bool OpenXRManager::CreateSwapchains() {
    uint32_t fmtCount = 0;
    if (!Check(xrEnumerateSwapchainFormats(m_session, 0, &fmtCount, nullptr),
               "xrEnumerateSwapchainFormats(count)") || fmtCount == 0) {
        return false;
    }
    std::vector<int64_t> formats(fmtCount, 0);
    xrEnumerateSwapchainFormats(m_session, fmtCount, &fmtCount, formats.data());

    // Prefer BGRA8 formats so CopyResource from our DXGI_FORMAT_B8G8R8A8_UNORM shared texture is direct.
    constexpr int64_t kPreferredFormats[] = {
        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };

    m_swapchainFormat = formats[0];
    for (int64_t pref : kPreferredFormats) {
        bool found = false;
        for (int64_t f : formats) {
            if (f == pref) {
                m_swapchainFormat = pref;
                found = true;
                break;
            }
        }
        if (found) break;
    }

    for (int eye = 0; eye < 2; ++eye) {
        uint32_t w = 0;
        uint32_t h = 0;
        ClampedEyeExtent(m_viewConfigs[eye], w, h);

        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format      = m_swapchainFormat;
        sci.sampleCount = 1;
        sci.width       = w;
        sci.height      = h;
        sci.faceCount   = 1;
        sci.arraySize   = 1;
        sci.mipCount    = 1;

        if (!Check(xrCreateSwapchain(m_session, &sci, &m_swapchains[eye]), "xrCreateSwapchain")) {
            return false;
        }

        uint32_t imgCount = 0;
        if (!Check(xrEnumerateSwapchainImages(m_swapchains[eye], 0, &imgCount, nullptr),
                   "xrEnumerateSwapchainImages(count)")) {
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> d3dImgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        if (!Check(xrEnumerateSwapchainImages(m_swapchains[eye], imgCount, &imgCount,
                                              reinterpret_cast<XrSwapchainImageBaseHeader*>(d3dImgs.data())),
                   "xrEnumerateSwapchainImages")) {
            return false;
        }

        m_swapchainImages[eye].clear();
        m_swapchainImages[eye].reserve(imgCount);
        for (const auto& img : d3dImgs) {
            m_swapchainImages[eye].push_back(img.texture);
        }

        LOG_INFO("OpenXR: swapchain[%d] created (%ux%u, format=%lld, images=%u)",
                 eye, w, h, static_cast<long long>(m_swapchainFormat), imgCount);
    }

    // Initialize the DX9-to-DX11 shared textures matching the clamped eye swapchain dimensions.
    uint32_t eyeW = 0;
    uint32_t eyeH = 0;
    ClampedEyeExtent(m_viewConfigs[0], eyeW, eyeH);
    GetDX9DX11Bridge().InitializeD3D11Resources(m_d3d11Device, m_d3d11Context, eyeW, eyeH);

    return true;
}

// Stylized open-right diamond crosshair with top/bottom spikes, hooked right ends, center orb & soft glow,
// matching the game's signature grapple reticle artwork. Shown as a quad layer along the right aim ray.
bool OpenXRManager::CreateReticleSwapchain(XrSwapchain& outSwapchain, bool lit) {
    constexpr uint32_t kSize = 256;
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    sci.usageFlags  = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format      = m_swapchainFormat;
    sci.sampleCount = 1;
    sci.width       = kSize;
    sci.height      = kSize;
    sci.faceCount   = 1;
    sci.arraySize   = 1;
    sci.mipCount    = 1;
    if (XR_FAILED(xrCreateSwapchain(m_session, &sci, &outSwapchain))) {
        sci.createFlags = 0;   // some runtimes reject static swapchains
        if (!Check(xrCreateSwapchain(m_session, &sci, &outSwapchain), "xrCreateSwapchain(reticle)")) {
            outSwapchain = XR_NULL_HANDLE;
            return false;
        }
    }
    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(outSwapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (imgCount == 0 || XR_FAILED(xrEnumerateSwapchainImages(outSwapchain, imgCount, &imgCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())))) {
        xrDestroySwapchain(outSwapchain);
        outSwapchain = XR_NULL_HANDLE;
        return false;
    }

    auto segSignedDist = [](float px, float py, float ax, float ay, float bx, float by,
                            float ra, float rb, float& outT) {
        const float abx = bx - ax, aby = by - ay;
        const float len2 = abx * abx + aby * aby;
        float t = 0.0f;
        if (len2 > 1e-6f) {
            t = std::clamp(((px - ax) * abx + (py - ay) * aby) / len2, 0.0f, 1.0f);
        }
        outT = t;
        const float qx = ax + abx * t;
        const float qy = ay + aby * t;
        const float dist = std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
        const float radius = ra + (rb - ra) * t;
        return dist - radius;
    };

    // Normalized geometry in [-1, 1]^2 (+Y down):
    // Bold open-right diamond with top/bottom spikes, hooked right ends, center orb & dark contrast outline.
    // Stays in the game's blue/cyan palette: cool muted steel-blue when idle, bright glowing white-cyan when grapple-ready.
    struct Seg { float ax, ay, bx, by, ra, rb; };
    const float wMain = lit ? 0.072f : 0.056f;
    const Seg kSegs[] = {
        {-0.60f,  0.00f,  0.00f, -0.56f, wMain,         wMain},         // Left -> Top
        {-0.60f,  0.00f,  0.00f,  0.56f, wMain,         wMain},         // Left -> Bottom
        { 0.00f, -0.56f,  0.46f, -0.14f, wMain,         wMain * 0.92f}, // Top -> Upper-Right elbow
        { 0.00f,  0.56f,  0.46f,  0.14f, wMain,         wMain * 0.92f}, // Bottom -> Lower-Right elbow
        { 0.00f, -0.56f,  0.00f, -0.94f, wMain * 0.95f, 0.012f},        // Top vertical spike
        { 0.00f,  0.56f,  0.00f,  0.94f, wMain * 0.95f, 0.012f},        // Bottom vertical spike
        { 0.46f, -0.14f,  0.72f, -0.34f, wMain * 0.88f, 0.012f},        // Upper-right outward barb
        { 0.46f,  0.14f,  0.72f,  0.34f, wMain * 0.88f, 0.012f},        // Lower-right outward barb
    };
    struct Sparkle { float x, y, r, strength; };
    const Sparkle kSparks[] = {
        {-0.38f, -0.34f, 0.016f, 0.80f}, {-0.26f, -0.46f, 0.013f, 0.65f}, {-0.50f, -0.18f, 0.014f, 0.70f},
        {-0.38f,  0.34f, 0.016f, 0.80f}, {-0.26f,  0.46f, 0.013f, 0.65f}, {-0.50f,  0.18f, 0.014f, 0.70f},
        { 0.24f, -0.58f, 0.014f, 0.75f}, { 0.36f, -0.46f, 0.016f, 0.85f}, { 0.56f, -0.34f, 0.013f, 0.70f},
        { 0.24f,  0.58f, 0.014f, 0.75f}, { 0.36f,  0.46f, 0.016f, 0.85f}, { 0.56f,  0.34f, 0.013f, 0.70f},
    };

    const bool bgra = (m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
    std::vector<uint8_t> px(kSize * kSize * 4, 0);
    const float c = (kSize - 1) * 0.5f;
    const float aa = 2.4f / c;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const float nx = (x - c) / c;
            const float ny = (y - c) / c;

            // Signed distance to the combined shape (segments + center orb)
            float minSd = std::sqrt(nx * nx + ny * ny) - (lit ? 0.125f : 0.095f);
            for (const auto& s : kSegs) {
                float t = 0.0f;
                const float sd = segSignedDist(nx, ny, s.ax, s.ay, s.bx, s.by, s.ra, s.rb, t);
                if (sd < minSd) minSd = sd;
            }

            const float core = std::clamp(0.5f - minSd / aa, 0.0f, 1.0f);
            // Dark navy contrast halo around the edges so both states stay legible against bright skyboxes,
            // while the luminous cyan aura blooms when grapple-ready.
            const float darkBand = (minSd > 0.0f) ? std::exp(-std::pow((minSd - 0.026f) / 0.026f, 2.0f)) * (lit ? 0.78f : 0.68f) : 0.0f;
            const float glow     = (minSd > 0.0f) ? std::exp(-minSd * (lit ? 10.5f : 15.0f)) * (lit ? 0.82f : 0.30f) : 0.0f;
            float spark = 0.0f;
            for (const auto& sp : kSparks) {
                const float dx = nx - sp.x, dy = ny - sp.y;
                const float sd = std::sqrt(dx * dx + dy * dy);
                if (sd < sp.r * 2.5f) {
                    spark = std::max(spark, std::clamp(1.0f - sd / sp.r, 0.0f, 1.0f) * sp.strength * (lit ? 1.00f : 0.30f));
                }
            }

            const float lightMix = std::clamp(core + spark + glow * (lit ? 0.65f : 0.45f), 0.0f, 1.0f);
            const float bodyA    = (core * (lit ? 1.00f : 0.78f)) + spark * 0.90f;
            const float a = std::clamp(std::max(bodyA, darkBand + glow * 0.60f), 0.0f, 1.0f);
            // Idle: calm steel/slate icy-blue; Lit: glowing white-aqua core with vibrant electric-cyan halo
            const float fgR = lit ? (120.0f + 135.0f * core) : (72.0f + 86.0f * core);
            const float fgG = lit ? (228.0f + 27.0f * core)  : (136.0f + 72.0f * core);
            const float fgB = lit ? 255.0f                   : (192.0f + 50.0f * core);
            const float bgR = lit ? 6.0f  : 5.0f;
            const float bgG = lit ? 20.0f : 10.0f;
            const float bgB = lit ? 34.0f : 22.0f;
            const float cr = bgR + (fgR - bgR) * lightMix;
            const float cg = bgG + (fgG - bgG) * lightMix;
            const float cb = bgB + (fgB - bgB) * lightMix;
            const uint8_t r = static_cast<uint8_t>(std::clamp(cr, 0.0f, 255.0f));
            const uint8_t g = static_cast<uint8_t>(std::clamp(cg, 0.0f, 255.0f));
            const uint8_t b = static_cast<uint8_t>(std::clamp(cb, 0.0f, 255.0f));

            uint8_t* p4 = &px[(y * kSize + x) * 4];
            p4[0] = bgra ? b : r;
            p4[1] = g;
            p4[2] = bgra ? r : b;
            p4[3] = static_cast<uint8_t>(a * 255.0f);
        }
    }

    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrAcquireSwapchainImage(outSwapchain, &acq, &idx)) ||
        XR_FAILED(xrWaitSwapchainImage(outSwapchain, &wait))) {
        xrDestroySwapchain(outSwapchain);
        outSwapchain = XR_NULL_HANDLE;
        return false;
    }
    if (idx < imgCount && imgs[idx].texture && m_d3d11Context) {
        m_d3d11Context->UpdateSubresource(imgs[idx].texture, 0, nullptr, px.data(), kSize * 4, 0);
    }
    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(outSwapchain, &rel);
    LOG_INFO("OpenXR: stylized hand reticle swapchain created (%ux%u, %s, imgs=%u, flags=0x%llx).",
             kSize, kSize, lit ? "lit" : "idle", imgCount, static_cast<unsigned long long>(sci.createFlags));
    return true;
}

bool OpenXRManager::CreateVrMenuSwapchain() {
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.createFlags = 0;
    sci.usageFlags  = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format      = m_swapchainFormat;
    sci.sampleCount = 1;
    sci.width       = VrSettingsMenu::kWidth;
    sci.height      = VrSettingsMenu::kHeight;
    sci.faceCount   = 1;
    sci.arraySize   = 1;
    sci.mipCount    = 1;
    if (!Check(xrCreateSwapchain(m_session, &sci, &m_vrMenuSwapchain), "xrCreateSwapchain(vrMenu)")) {
        m_vrMenuSwapchain = XR_NULL_HANDLE;
        return false;
    }
    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(m_vrMenuSwapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    if (imgCount == 0 || XR_FAILED(xrEnumerateSwapchainImages(m_vrMenuSwapchain, imgCount, &imgCount,
                                       reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data())))) {
        xrDestroySwapchain(m_vrMenuSwapchain);
        m_vrMenuSwapchain = XR_NULL_HANDLE;
        return false;
    }
    m_vrMenuImages.clear();
    m_vrMenuImages.reserve(imgCount);
    for (const auto& img : imgs) m_vrMenuImages.push_back(img.texture);
    m_vrMenu.MarkDirty();
    LOG_INFO("OpenXR: VR Settings Menu swapchain created (%ux%u, images=%u).",
             VrSettingsMenu::kWidth, VrSettingsMenu::kHeight, imgCount);
    return true;
}

bool OpenXRManager::CreateSpaces() {
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace = math::IdentityPose();
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!Check(xrCreateReferenceSpace(m_session, &rs, &m_baseSpace), "xrCreateReferenceSpace(LOCAL)"))
        return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(m_session, &rs, &m_viewSpace), "xrCreateReferenceSpace(VIEW)"))
        return false;
    return true;
}

bool OpenXRManager::CreateActions() {
    xrStringToPath(m_instance, "/user/hand/left", &m_handPaths[0]);
    xrStringToPath(m_instance, "/user/hand/right", &m_handPaths[1]);

    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    if (!Check(xrCreateActionSet(m_instance, &asci, &m_actionSet), "xrCreateActionSet")) return false;

    auto makeAction = [&](XrActionType type, const char* name, const char* loc, XrAction& out) {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        aci.actionType = type;
        std::strcpy(aci.actionName, name);
        std::strcpy(aci.localizedActionName, loc);
        aci.countSubactionPaths = 2;
        aci.subactionPaths = m_handPaths;
        return Check(xrCreateAction(m_actionSet, &aci, &out), name);
    };
    if (!makeAction(XR_ACTION_TYPE_POSE_INPUT,     "aim_pose",      "Aim Pose",       m_aimPoseAction))      return false;
    if (!makeAction(XR_ACTION_TYPE_POSE_INPUT,     "grip_pose",     "Grip Pose",      m_gripPoseAction))     return false;
    if (!makeAction(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick",    "Thumbstick",     m_thumbstickAction))   return false;
    if (!makeAction(XR_ACTION_TYPE_FLOAT_INPUT,    "trigger_value", "Trigger Value",  m_triggerValueAction)) return false;
    if (!makeAction(XR_ACTION_TYPE_FLOAT_INPUT,    "squeeze_value", "Grip Squeeze",   m_squeezeValueAction)) return false;
    if (!makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,  "jump_button",    "Jump / Boost",     m_jumpButtonAction))   return false;
    if (!makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,  "menu_button",    "Menu / Back",      m_menuButtonAction))   return false;
    if (!makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,  "vr_menu_button", "VR Settings Menu", m_vrMenuButtonAction)) return false;
    if (!makeAction(XR_ACTION_TYPE_BOOLEAN_INPUT,  "sprint_click",   "Sprint Click (L3)", m_sprintClickAction)) return false;

    auto path = [&](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(m_instance, s, &p);
        return p;
    };
    for (const auto& prof : kProfiles) {
        std::vector<XrActionSuggestedBinding> b = {
            {m_gripPoseAction, path("/user/hand/left/input/grip/pose")},
            {m_gripPoseAction, path("/user/hand/right/input/grip/pose")},
            {m_aimPoseAction,  path("/user/hand/left/input/aim/pose")},
            {m_aimPoseAction,  path("/user/hand/right/input/aim/pose")},
        };

        std::string_view pName(prof.profile);
        if (pName.find("oculus/touch_controller") != std::string_view::npos) {
            b.push_back({m_thumbstickAction,   path("/user/hand/left/input/thumbstick")});
            b.push_back({m_thumbstickAction,   path("/user/hand/right/input/thumbstick")});
            b.push_back({m_triggerValueAction, path("/user/hand/left/input/trigger/value")});
            b.push_back({m_triggerValueAction, path("/user/hand/right/input/trigger/value")});
            b.push_back({m_squeezeValueAction, path("/user/hand/left/input/squeeze/value")});
            b.push_back({m_squeezeValueAction, path("/user/hand/right/input/squeeze/value")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/right/input/a/click")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/left/input/x/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/left/input/menu/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/right/input/b/click")});
            b.push_back({m_vrMenuButtonAction, path("/user/hand/right/input/thumbstick/click")});
            b.push_back({m_sprintClickAction,  path("/user/hand/left/input/thumbstick/click")});
        } else if (pName.find("valve/index_controller") != std::string_view::npos) {
            b.push_back({m_thumbstickAction,   path("/user/hand/left/input/thumbstick")});
            b.push_back({m_thumbstickAction,   path("/user/hand/right/input/thumbstick")});
            b.push_back({m_triggerValueAction, path("/user/hand/left/input/trigger/value")});
            b.push_back({m_triggerValueAction, path("/user/hand/right/input/trigger/value")});
            b.push_back({m_squeezeValueAction, path("/user/hand/left/input/squeeze/value")});
            b.push_back({m_squeezeValueAction, path("/user/hand/right/input/squeeze/value")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/right/input/a/click")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/left/input/a/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/left/input/b/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/right/input/b/click")});
            b.push_back({m_vrMenuButtonAction, path("/user/hand/right/input/thumbstick/click")});
            b.push_back({m_sprintClickAction,  path("/user/hand/left/input/thumbstick/click")});
        } else if (pName.find("microsoft/motion_controller") != std::string_view::npos) {
            b.push_back({m_thumbstickAction,   path("/user/hand/left/input/thumbstick")});
            b.push_back({m_thumbstickAction,   path("/user/hand/right/input/thumbstick")});
            b.push_back({m_triggerValueAction, path("/user/hand/left/input/trigger/value")});
            b.push_back({m_triggerValueAction, path("/user/hand/right/input/trigger/value")});
            b.push_back({m_squeezeValueAction, path("/user/hand/left/input/squeeze/click")});
            b.push_back({m_squeezeValueAction, path("/user/hand/right/input/squeeze/click")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/left/input/trackpad/click")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/right/input/trackpad/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/left/input/menu/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/right/input/menu/click")});
            b.push_back({m_vrMenuButtonAction, path("/user/hand/right/input/thumbstick/click")});
            b.push_back({m_sprintClickAction,  path("/user/hand/left/input/thumbstick/click")});
        } else if (pName.find("htc/vive_controller") != std::string_view::npos) {
            b.push_back({m_thumbstickAction,   path("/user/hand/left/input/trackpad")});
            b.push_back({m_thumbstickAction,   path("/user/hand/right/input/trackpad")});
            b.push_back({m_triggerValueAction, path("/user/hand/left/input/trigger/value")});
            b.push_back({m_triggerValueAction, path("/user/hand/right/input/trigger/value")});
            b.push_back({m_squeezeValueAction, path("/user/hand/left/input/squeeze/click")});
            b.push_back({m_squeezeValueAction, path("/user/hand/right/input/squeeze/click")});
            b.push_back({m_jumpButtonAction,   path("/user/hand/right/input/trackpad/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/left/input/menu/click")});
            b.push_back({m_menuButtonAction,   path("/user/hand/right/input/menu/click")});
        } else if (pName.find("khr/simple_controller") != std::string_view::npos) {
            b.push_back({m_triggerValueAction, path("/user/hand/left/input/select/click")});
            b.push_back({m_triggerValueAction, path("/user/hand/right/input/select/click")});
            b.push_back({m_squeezeValueAction, path("/user/hand/left/input/menu/click")});
            b.push_back({m_squeezeValueAction, path("/user/hand/right/input/menu/click")});
        }

        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = path(prof.profile);
        sb.suggestedBindings = b.data();
        sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
        XrResult r = xrSuggestInteractionProfileBindings(m_instance, &sb);
        if (XR_FAILED(r)) LOG_WARN("OpenXR: bindings for %s rejected (%d)", prof.profile, static_cast<int>(r));
    }

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.poseInActionSpace = math::IdentityPose();
        sci.subactionPath = m_handPaths[h];
        sci.action = m_aimPoseAction;
        if (!Check(xrCreateActionSpace(m_session, &sci, &m_aimSpaces[h]), "xrCreateActionSpace(aim)")) return false;
        sci.action = m_gripPoseAction;
        if (!Check(xrCreateActionSpace(m_session, &sci, &m_gripSpaces[h]), "xrCreateActionSpace(grip)")) return false;
    }

    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &m_actionSet;
    return Check(xrAttachSessionActionSets(m_session, &ai), "xrAttachSessionActionSets");
}

void OpenXRManager::Shutdown() {
    DX9DX11Bridge::BindOpenXRManager(nullptr);
    GetDX9DX11Bridge().Shutdown();

    if (m_session != XR_NULL_HANDLE && m_sessionRunning) xrEndSession(m_session);
    m_sessionRunning = false;
    for (int eye = 0; eye < 2; ++eye) {
        m_swapchainImages[eye].clear();
        if (m_swapchains[eye]) {
            xrDestroySwapchain(m_swapchains[eye]);
            m_swapchains[eye] = XR_NULL_HANDLE;
        }
    }
    for (int h = 0; h < 2; ++h) {
        if (m_aimSpaces[h]) { xrDestroySpace(m_aimSpaces[h]); m_aimSpaces[h] = XR_NULL_HANDLE; }
        if (m_gripSpaces[h]) { xrDestroySpace(m_gripSpaces[h]); m_gripSpaces[h] = XR_NULL_HANDLE; }
    }
    if (m_reticleSwapchain) { xrDestroySwapchain(m_reticleSwapchain); m_reticleSwapchain = XR_NULL_HANDLE; }
    if (m_reticleLitSwapchain) { xrDestroySwapchain(m_reticleLitSwapchain); m_reticleLitSwapchain = XR_NULL_HANDLE; }
    m_vrMenuImages.clear();
    if (m_vrMenuSwapchain) { xrDestroySwapchain(m_vrMenuSwapchain); m_vrMenuSwapchain = XR_NULL_HANDLE; }
    if (m_viewSpace) { xrDestroySpace(m_viewSpace); m_viewSpace = XR_NULL_HANDLE; }
    if (m_baseSpace) { xrDestroySpace(m_baseSpace); m_baseSpace = XR_NULL_HANDLE; }
    if (m_actionSet) { xrDestroyActionSet(m_actionSet); m_actionSet = XR_NULL_HANDLE; }
    m_aimPoseAction = m_gripPoseAction = XR_NULL_HANDLE;
    m_thumbstickAction = m_triggerValueAction = m_squeezeValueAction = m_jumpButtonAction = XR_NULL_HANDLE;
    m_menuButtonAction = m_vrMenuButtonAction = XR_NULL_HANDLE;
    if (m_session) { xrDestroySession(m_session); m_session = XR_NULL_HANDLE; }
    if (m_instance) { xrDestroyInstance(m_instance); m_instance = XR_NULL_HANDLE; }
    m_systemId = XR_NULL_SYSTEM_ID;
    m_state = XR_SESSION_STATE_UNKNOWN;
    SafeRelease(m_d3d11Context);
    SafeRelease(m_d3d11Device);
}

bool OpenXRManager::PollEvents() {
    if (!IsInitialized()) return false;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(m_instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            HandleStateChange(*reinterpret_cast<XrEventDataSessionStateChanged*>(&ev));
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            LOG_WARN("OpenXR: instance loss pending.");
            return false;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
            for (int h = 0; h < 2; ++h) {
                XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, m_handPaths[h], &st))) {
                    char buf[XR_MAX_PATH_LENGTH] = "(none)";
                    uint32_t len = 0;
                    if (st.interactionProfile != XR_NULL_PATH)
                        xrPathToString(m_instance, st.interactionProfile, sizeof(buf), &len, buf);
                    LOG_INFO("OpenXR: %s hand interaction profile -> %s", h ? "right" : "left", buf);
                }
            }
            break;
        }
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    return !(m_state == XR_SESSION_STATE_EXITING || m_state == XR_SESSION_STATE_LOSS_PENDING);
}

void OpenXRManager::HandleStateChange(const XrEventDataSessionStateChanged& e) {
    LOG_INFO("OpenXR: session state %s -> %s", SessionStateName(m_state), SessionStateName(e.state));
    m_state = e.state;
    switch (e.state) {
    case XR_SESSION_STATE_READY: {
        XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
        bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        if (Check(xrBeginSession(m_session, &bi), "xrBeginSession")) {
            m_sessionRunning = true;
            LOG_INFO("OpenXR: session running.");
        }
        break;
    }
    case XR_SESSION_STATE_STOPPING:
        Check(xrEndSession(m_session), "xrEndSession");
        m_sessionRunning = false;
        break;
    default:
        break;
    }
}

bool OpenXRManager::AcquireAndBlitEyeSwapchain(int swapIdx, int slot, DX9DX11Bridge::SlotInfo* copied) {
    if (swapIdx < 0 || swapIdx >= 2 || !m_swapchains[swapIdx] || m_swapchainImages[swapIdx].empty()) return false;

    uint32_t imgIdx = 0;
    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!Check(xrAcquireSwapchainImage(m_swapchains[swapIdx], &acq, &imgIdx), "xrAcquireSwapchainImage")) {
        return false;
    }

    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (!Check(xrWaitSwapchainImage(m_swapchains[swapIdx], &wait), "xrWaitSwapchainImage")) {
        XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(m_swapchains[swapIdx], &rel);
        return false;
    }

    ID3D11Texture2D* dstTex = m_swapchainImages[swapIdx][imgIdx];
    if (dstTex) {
        GetDX9DX11Bridge().CopySharedEyeToD3D11Texture(slot, dstTex, copied);
    }

    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    Check(xrReleaseSwapchainImage(m_swapchains[swapIdx], &rel), "xrReleaseSwapchainImage");
    return true;
}

namespace {

constexpr float kVrMenuDistM   = 1.45f;
constexpr float kVrMenuWidthM  = 1.05f;
constexpr float kVrMenuHeightM = kVrMenuWidthM * static_cast<float>(VrSettingsMenu::kHeight) / static_cast<float>(VrSettingsMenu::kWidth);

XrPosef PlaceYawQuadInFrontOfHead(const XrPosef& headPose, float distM, float* outYawDeg = nullptr) {
    const XrQuaternionf& q = headPose.orientation;
    const float fx = -(2.0f * (q.x * q.z + q.w * q.y));
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float len = std::sqrt(fx * fx + fz * fz);
    float dx = 0.0f, dz = -1.0f;
    if (len > 1e-3f) { dx = fx / len; dz = fz / len; }
    const float yaw = std::atan2(-dx, -dz);
    if (outYawDeg) *outYawDeg = yaw * 180.0f / 3.14159265f;
    XrPosef out{};
    out.orientation = XrQuaternionf{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
    out.position    = XrVector3f{headPose.position.x + dx * distM,
                                 headPose.position.y,
                                 headPose.position.z + dz * distM};
    return out;
}

// Places a virtual screen along the user's current gaze yaw + comfortable clamped pitch so pausing
// while looking up/down opens the menu right where you're looking rather than snapping far away.
XrPosef PlaceGazeQuadInFrontOfHead(const XrPosef& headPose, float distM, float maxPitchDeg = 22.0f) {
    const XrVector3f fwd = math::RotateVector(headPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
    const float hLen = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    float ux = 0.0f, uz = -1.0f;
    if (hLen > 1e-3f) { ux = fwd.x / hLen; uz = fwd.z / hLen; }
    const float yaw = std::atan2(-ux, -uz);
    const float rawPitch = std::atan2(fwd.y, std::max(1e-3f, hLen));
    const float maxPitch = maxPitchDeg * 3.14159265f / 180.0f;
    const float pitch = std::clamp(rawPitch * 0.65f, -maxPitch, maxPitch);

    const float cy = std::cos(yaw * 0.5f),   sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    XrPosef out{};
    // q_yaw * q_pitch (level horizon roll = 0)
    out.orientation = math::Normalize(XrQuaternionf{cy * sp, sy * cp, -sy * sp, cy * cp});
    const XrVector3f dir = math::RotateVector(out.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
    out.position = XrVector3f{headPose.position.x + dir.x * distM,
                              headPose.position.y + dir.y * distM,
                              headPose.position.z + dir.z * distM};
    return out;
}

XrPosef PlaceFullViewQuadAtHead(const XrPosef& headPose, float distM) {
    const XrVector3f dir = math::RotateVector(headPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
    XrPosef out{};
    out.orientation = math::Normalize(headPose.orientation);
    out.position    = XrVector3f{headPose.position.x + dir.x * distM,
                                 headPose.position.y + dir.y * distM,
                                 headPose.position.z + dir.z * distM};
    return out;
}

XrPosef BlendPoses(const XrPosef& a, const XrPosef& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    XrPosef out{};
    out.position = XrVector3f{a.position.x + (b.position.x - a.position.x) * t,
                              a.position.y + (b.position.y - a.position.y) * t,
                              a.position.z + (b.position.z - a.position.z) * t};
    const float dot = a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y +
                      a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w;
    const float sign = (dot < 0.0f) ? -1.0f : 1.0f;
    out.orientation = math::Normalize(XrQuaternionf{
        a.orientation.x + (sign * b.orientation.x - a.orientation.x) * t,
        a.orientation.y + (sign * b.orientation.y - a.orientation.y) * t,
        a.orientation.z + (sign * b.orientation.z - a.orientation.z) * t,
        a.orientation.w + (sign * b.orientation.w - a.orientation.w) * t});
    return out;
}

} // namespace

bool OpenXRManager::RayHitQuad(const XrPosef& aimPose, const XrPosef& quadPose,
                               float widthM, float heightM, float& u, float& v) {
    const XrQuaternionf& q = quadPose.orientation;
    const XrQuaternionf qInv{-q.x, -q.y, -q.z, q.w};
    // Ray origin & direction (aim forward = -Z) in the quad's local frame (quad lies in its XY plane).
    const XrVector3f rel{aimPose.position.x - quadPose.position.x,
                         aimPose.position.y - quadPose.position.y,
                         aimPose.position.z - quadPose.position.z};
    const XrVector3f o = math::RotateVector(qInv, rel);
    const XrVector3f d = math::RotateVector(qInv, math::RotateVector(aimPose.orientation, XrVector3f{0.0f, 0.0f, -1.0f}));
    if (std::fabs(d.z) < 1e-4f) return false;
    const float t = -o.z / d.z;
    if (t <= 0.0f) return false;
    const float x = o.x + d.x * t;
    const float y = o.y + d.y * t;
    u = x / widthM + 0.5f;
    v = 0.5f - y / heightM;
    return u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f;
}

bool OpenXRManager::RayHitFlatScreen(const XrPosef& aimPose, float& u, float& v) const {
    const float widthM  = std::clamp(m_quadCurWidthM, 0.5f, 30.0f);
    const float heightM = widthM * GetDX9DX11Bridge().LastBackBufferAspectHOverW();
    return RayHitQuad(aimPose, m_quadPose, widthM, heightM, u, v);
}

void OpenXRManager::SetVrMenuOpen(bool open) {
    if (m_vrMenuOpen.exchange(open, std::memory_order_relaxed) == open) return;
    std::lock_guard lock(m_vrMenuMutex);
    m_vrMenu.SetOpen(open);
    if (open) {
        m_vrMenuNeedsPlacement.store(true, std::memory_order_relaxed);
    }
    LOG_INFO("VrMenu: %s", open ? "OPENED" : "closed");
}

VrMenuUpdateResult OpenXRManager::UpdateVrMenu(const TrackingState& s) {
    if (!IsVrMenuOpen()) return {};
    float u = 0.5f, v = 0.5f;
    const bool hit = s.rightAim.positionValid && s.rightAim.orientationValid &&
                     RayHitQuad(s.rightAim.pose, m_vrMenuPose, kVrMenuWidthM, kVrMenuHeightM, u, v);
    VrMenuUpdateResult res;
    {
        std::lock_guard lock(m_vrMenuMutex);
        res = m_vrMenu.Update(s.input, hit, u, v);
    }
    if (res.recenterRequested) {
        m_vrMenuNeedsPlacement.store(true, std::memory_order_relaxed);
        m_quadNeedsPlacement.store(true, std::memory_order_relaxed);
    }
    if (res.closeRequested) {
        SetVrMenuOpen(false);
    }
    return res;
}

bool OpenXRManager::LookupPoseSample(uint64_t id, PoseSample& out) const {
    if (id == 0) return false;
    std::lock_guard lock(m_stateMutex);
    const PoseSample& ps = m_poseHistory[id % kPoseHistory];
    if (ps.id != id) return false;
    out = ps;
    return true;
}

bool OpenXRManager::RunFrame() {
    if (!m_sessionRunning) return false;

    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (!Check(xrWaitFrame(m_session, &wi, &fs), "xrWaitFrame")) return false;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (!Check(xrBeginFrame(m_session, &bi), "xrBeginFrame")) return false;

    XrActiveActionSet active{m_actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    XrResult sr = xrSyncActions(m_session, &si); // XR_SESSION_NOT_FOCUSED is a success code
    if (XR_FAILED(sr)) Check(sr, "xrSyncActions");

    LocateAll(fs.predictedDisplayTime);

    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView projViews[2]{
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad reticle{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad vrMenuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[3] = {nullptr, nullptr, nullptr};
    uint32_t layerCount = 0;

    bool submitLayer = false;
    const auto& cfg = samu::config::Get();
    if (fs.shouldRender && cfg.enableStereo && m_swapchains[0]) {
        TrackingState ts = GetTrackingState();
        auto& bridge = GetDX9DX11Bridge();
        DX9DX11Bridge::SlotInfo slots[2] = {bridge.GetSlotInfo(0), bridge.GetSlotInfo(1)};
        const bool immersiveReq = m_immersive.load(std::memory_order_relaxed);
        // AFR stereo: slot 0 holds the latest LEFT-eye frame and slot 1 the latest RIGHT-eye frame, written by
        // consecutive game frames.
        const uint64_t serialGap = (slots[0].serial > slots[1].serial) ? slots[0].serial - slots[1].serial
                                                                        : slots[1].serial - slots[0].serial;
        bool afr = immersiveReq && (m_displayBlend >= 0.999f) && m_swapchains[1] &&
                   slots[0].tagged && slots[1].tagged && slots[0].eye == 0 && slots[1].eye == 1 && serialGap <= 2;
        bool blitOk = ts.viewsValid && AcquireAndBlitEyeSwapchain(0, 0, &slots[0]);
        if (blitOk && afr) afr = AcquireAndBlitEyeSwapchain(1, 1, &slots[1]);
        // Re-validate with the info captured atomically with each copy.
        if (afr) afr = slots[0].eye == 0 && slots[1].eye == 1 && slots[0].tagged && slots[1].tagged;
        if (blitOk) {
            const float aspectHW = bridge.LastBackBufferAspectHOverW();
            const XrRect2Di fullRect{{0, 0}, {static_cast<int32_t>(ts.views[0].width),
                                              static_cast<int32_t>(ts.views[0].height)}};

            const auto nowBlend = std::chrono::steady_clock::now();
            float dtBlend = 0.011f;
            if (m_lastBlendTime.time_since_epoch().count() != 0) {
                dtBlend = std::clamp(std::chrono::duration<float>(nowBlend - m_lastBlendTime).count(), 0.001f, 0.05f);
            }
            m_lastBlendTime = nowBlend;

            // Compute game projection tangents & current HMD head pose for both immersive mode and smooth morphs.
            const float hHalf = std::clamp(m_gameFovDeg.load(std::memory_order_relaxed), 40.0f, 150.0f)
                                * 0.5f * 3.14159265358979323846f / 180.0f;
            const float viewScale = samu::config::GetViewScale();
            float tanH = std::tan(hHalf) * viewScale;
            float tanV = tanH * aspectHW;
            // m_measTanH/m_measTanV is measured from the game's actual camera projection (which already incorporates
            // ViewScale via VrFovOr), so matching it directly guarantees 1:1 head-turn angular lock without skew.
            const float mH = m_measTanH.load(std::memory_order_relaxed);
            const float mV = m_measTanV.load(std::memory_order_relaxed);
            if (mH > 0.0f && mV > 0.0f && std::fabs((mV / mH) - aspectHW) < 0.08f) {
                tanH = mH;
                tanV = mV;
            }
            XrPosef headPose = ts.hmd.orientationValid ? ts.hmd.pose : ts.views[0].pose;
            if (!ts.hmd.positionValid) {
                headPose.position.x = 0.5f * (ts.views[0].pose.position.x + ts.views[1].pose.position.x);
                headPose.position.y = 0.5f * (ts.views[0].pose.position.y + ts.views[1].pose.position.y);
                headPose.position.z = 0.5f * (ts.views[0].pose.position.z + ts.views[1].pose.position.z);
            }

            constexpr float kTransitionSec = 0.24f;
            constexpr float kFullQuadDistM = 1.55f;
            const float targetDistM  = std::clamp(cfg.flatScreenDistanceM, 0.5f, 20.0f);
            const float targetWidthM = std::clamp(cfg.flatScreenWidthM, 0.5f, 30.0f);

            if (immersiveReq != m_prevImmersiveTarget) {
                if (ts.hmd.orientationValid) {
                    if (!immersiveReq) {
                        // Entering Pause / Menu from VR: start full-FOV right on current gaze and glide smoothly back.
                        m_quadStartPose  = PlaceFullViewQuadAtHead(headPose, kFullQuadDistM);
                        m_quadTargetPose = PlaceGazeQuadInFrontOfHead(headPose, targetDistM, 22.0f);
                        m_quadNeedsPlacement.store(false, std::memory_order_relaxed);
                    } else {
                        // Returning to VR: glide smoothly forward from current floating screen into full-FOV VR.
                        m_quadStartPose  = m_quadPose;
                        m_quadTargetPose = PlaceFullViewQuadAtHead(headPose, kFullQuadDistM);
                    }
                }
                m_prevImmersiveTarget = immersiveReq;
            }
            if (!immersiveReq && m_quadNeedsPlacement.exchange(false, std::memory_order_relaxed) && ts.hmd.orientationValid) {
                m_quadTargetPose = (m_displayBlend > 0.01f)
                                       ? PlaceGazeQuadInFrontOfHead(headPose, targetDistM, 22.0f)
                                       : PlaceYawQuadInFrontOfHead(headPose, targetDistM);
                if (m_displayBlend <= 0.001f) {
                    m_quadPose = m_quadTargetPose;
                    LOG_INFO("OpenXR: flat virtual screen placed %.2f m ahead (%.2f m wide).",
                             targetDistM, targetWidthM);
                }
            }

            const float blendStep = dtBlend / kTransitionSec;
            m_displayBlend = immersiveReq ? std::min(1.0f, m_displayBlend + blendStep)
                                          : std::max(0.0f, m_displayBlend - blendStep);
            const bool useProjection = immersiveReq && (m_displayBlend >= 0.999f);

            if (immersiveReq != m_lastSubmittedImmersive) {
                LOG_INFO("OpenXR: display mode -> %s", immersiveReq ? "IMMERSIVE (projection, game-matched FOV)"
                                                                    : "FLAT VIRTUAL SCREEN (quad)");
                m_lastSubmittedImmersive = immersiveReq;
            }

            if (useProjection) {
                // Mono game frame -> identical image for both eyes from the head-center pose with the exact
                // FOV the game rendered (horizontal FOV + backbuffer aspect). No per-eye crop => no doubling.
                const float hHalfDisp = std::atan(tanH);
                const float vHalf     = std::atan(tanV);
                bool matched = false;
                for (int eye = 0; eye < 2; ++eye) {
                    const int slot = afr ? eye : 0;
                    // Default: the current head pose (zero disparity). With render-pose matching, use the exact pose
                    // the game rendered this image with, so the compositor's reprojection corrects head motion
                    // since then instead of the image "swimming" with the head (62 Hz game vs 90 Hz HMD judder).
                    XrPosef pose = headPose;
                    PoseSample ps;
                    if (cfg.matchRenderPose && slots[slot].tagged && LookupPoseSample(slots[slot].sampleId, ps) && ps.headValid) {
                        pose = ps.head;
                        matched = true;
                        if (afr && ps.eyesValid) {
                            const float k = cfg.ipdScale;
                            pose.position.x = ps.head.position.x + (ps.eye[eye].position.x - ps.head.position.x) * k;
                            pose.position.y = ps.head.position.y + (ps.eye[eye].position.y - ps.head.position.y) * k;
                            pose.position.z = ps.head.position.z + (ps.eye[eye].position.z - ps.head.position.z) * k;
                        }
                    }
                    projViews[eye].pose = pose;
                    projViews[eye].fov  = XrFovf{-hHalfDisp, hHalfDisp, vHalf, -vHalf};
                    projViews[eye].subImage.swapchain       = m_swapchains[afr ? eye : 0];
                    projViews[eye].subImage.imageRect       = fullRect;
                    projViews[eye].subImage.imageArrayIndex = 0;
                }
                if (matched) ++m_poseMatchedFrames;
                if (afr) ++m_afrStereoFrames;
                layer.space     = m_baseSpace;
                layer.viewCount = 2;
                layer.views     = projViews;
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
                layerCount = 1;

                // Hand reticle: a quad on the right controller's aim ray at the distance of whatever the ray hits
                // in the game world (traced by the bridge), facing the viewer. The game's aim is pointed at that
                // same spot, so this marks where the grapple will go. It brightens in cyan when grapple-able.
                if (m_reticleSwapchain && m_handReticleVisible.load(std::memory_order_relaxed) &&
                    ts.rightAim.positionValid && ts.rightAim.orientationValid && ts.hmd.orientationValid) {
                    const float wantDist = std::clamp(m_handReticleDistM.load(std::memory_order_relaxed), 0.6f, 60.0f);
                    m_reticleDistSmooth += (wantDist - m_reticleDistSmooth) * 0.4f;
                    const float kDist = m_reticleDistSmooth;
                    const bool  lit   = m_handReticleLit.load(std::memory_order_relaxed) && m_reticleLitSwapchain;
                    const XrVector3f fwd = math::RotateVector(ts.rightAim.pose.orientation, XrVector3f{0.0f, 0.0f, -1.0f});
                    reticle.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                         XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    reticle.space         = m_baseSpace;
                    reticle.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    reticle.subImage.swapchain       = lit ? m_reticleLitSwapchain : m_reticleSwapchain;
                    reticle.subImage.imageRect       = XrRect2Di{{0, 0}, {256, 256}};
                    reticle.subImage.imageArrayIndex = 0;
                    reticle.pose.orientation = ts.hmd.pose.orientation;
                    reticle.pose.position    = XrVector3f{ts.rightAim.pose.position.x + fwd.x * kDist,
                                                          ts.rightAim.pose.position.y + fwd.y * kDist,
                                                          ts.rightAim.pose.position.z + fwd.z * kDist};
                    const float reticleSize = kDist * (lit ? 0.112f : 0.086f) * samu::config::GetReticleScale();
                    reticle.size = XrExtent2Df{reticleSize, reticleSize};
                    layers[1] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&reticle);
                    layerCount = 2;
                    static bool s_reticleLogged = false;
                    if (!s_reticleLogged) {
                        s_reticleLogged = true;
                        LOG_INFO("OpenXR: first hand reticle quad submitted (dist=%.2f m, size=%.2f m, lit=%d, pos=(%.2f, %.2f, %.2f)).",
                                 kDist, reticleSize, lit ? 1 : 0,
                                 reticle.pose.position.x, reticle.pose.position.y, reticle.pose.position.z);
                    }
                }
            } else {
                // Smooth morph between full-FOV VR gaze and comfortable resting 2D virtual screen (240ms smoothstep).
                const float t = m_displayBlend;
                const float s = t * t * (3.0f - 2.0f * t);
                const float fullWidthM = 2.0f * kFullQuadDistM * std::max(0.4f, tanH);
                if (immersiveReq && ts.hmd.orientationValid) {
                    m_quadTargetPose = PlaceFullViewQuadAtHead(headPose, kFullQuadDistM);
                }
                if (t <= 0.001f) {
                    m_quadPose      = m_quadTargetPose;
                    m_quadCurWidthM = targetWidthM;
                } else if (immersiveReq) {
                    m_quadPose      = BlendPoses(m_quadStartPose, m_quadTargetPose, s);
                    m_quadCurWidthM = targetWidthM + (fullWidthM - targetWidthM) * s;
                } else {
                    m_quadPose      = BlendPoses(m_quadTargetPose, m_quadStartPose, s);
                    m_quadCurWidthM = targetWidthM + (fullWidthM - targetWidthM) * s;
                }
                quad.layerFlags    = 0;
                quad.space         = m_baseSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain       = m_swapchains[0];
                quad.subImage.imageRect       = fullRect;
                quad.subImage.imageArrayIndex = 0;
                quad.pose = m_quadPose;
                quad.size = XrExtent2Df{m_quadCurWidthM, m_quadCurWidthM * aspectHW};
                layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
                layerCount = 1;
            }

            // Overlay in-headset VR Settings Menu quad on top when open.
            if (m_vrMenuOpen.load(std::memory_order_relaxed) && m_vrMenuSwapchain && layerCount < 3) {
                if (m_vrMenuNeedsPlacement.exchange(false, std::memory_order_relaxed) && ts.hmd.orientationValid) {
                    m_vrMenuPose = PlaceYawQuadInFrontOfHead(ts.hmd.pose, kVrMenuDistM);
                }
                bool uploaded = false;
                {
                    std::lock_guard lock(m_vrMenuMutex);
                    const bool bgra = (m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                                       m_swapchainFormat == DXGI_FORMAT_B8G8R8A8_UNORM);
                    if (m_vrMenu.RenderIfDirty(bgra, m_vrMenuPixels)) {
                        uint32_t idx = 0;
                        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                        wait.timeout = XR_INFINITE_DURATION;
                        if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_vrMenuSwapchain, &acq, &idx)) &&
                            XR_SUCCEEDED(xrWaitSwapchainImage(m_vrMenuSwapchain, &wait))) {
                            if (idx < m_vrMenuImages.size() && m_vrMenuImages[idx] && m_d3d11Context) {
                                m_d3d11Context->UpdateSubresource(
                                    m_vrMenuImages[idx], 0, nullptr, m_vrMenuPixels.data(), VrSettingsMenu::kWidth * 4, 0);
                                uploaded = true;
                            }
                            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                            xrReleaseSwapchainImage(m_vrMenuSwapchain, &rel);
                        }
                    }
                }
                (void)uploaded;
                vrMenuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                vrMenuQuad.space         = m_baseSpace;
                vrMenuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                vrMenuQuad.subImage.swapchain       = m_vrMenuSwapchain;
                vrMenuQuad.subImage.imageRect       = XrRect2Di{{0, 0}, {static_cast<int32_t>(VrSettingsMenu::kWidth),
                                                                          static_cast<int32_t>(VrSettingsMenu::kHeight)}};
                vrMenuQuad.subImage.imageArrayIndex = 0;
                vrMenuQuad.pose = m_vrMenuPose;
                vrMenuQuad.size = XrExtent2Df{kVrMenuWidthM, kVrMenuHeightM};
                layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&vrMenuQuad);
            }
            submitLayer = true;
            m_submittedStereoFrames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime          = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount           = submitLayer ? layerCount : 0u;
    ei.layers               = submitLayer ? layers : nullptr;
    if (!Check(xrEndFrame(m_session, &ei), "xrEndFrame")) return false;

    ++m_frameCount;
    return true;
}

void OpenXRManager::SetDisplayMode(bool immersive, float gameHorizontalFovDeg) {
    m_immersive.store(immersive, std::memory_order_relaxed);
    if (gameHorizontalFovDeg > 30.0f && gameHorizontalFovDeg < 160.0f) {
        m_gameFovDeg.store(gameHorizontalFovDeg, std::memory_order_relaxed);
    }
}

void OpenXRManager::Locate(XrSpace space, XrTime time, TrackedPose& out) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(space, m_baseSpace, time, &loc))) {
        out.positionValid = out.orientationValid = false;
        return;
    }
    out.positionValid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    out.orientationValid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
    out.positionTracked = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
    out.orientationTracked = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
    if (out.positionValid || out.orientationValid) out.pose = loc.pose;
}

void OpenXRManager::LocateAll(XrTime time) {
    TrackingState s;
    s.displayTime = time;
    s.sampleId    = ++m_sampleCounter;
    Locate(m_viewSpace, time, s.hmd);
    Locate(m_aimSpaces[0], time, s.leftAim);
    Locate(m_gripSpaces[0], time, s.leftGrip);
    Locate(m_aimSpaces[1], time, s.rightAim);
    Locate(m_gripSpaces[1], time, s.rightGrip);

    // Phase 4: Query OpenXR controller action states (Left/Right Thumbsticks, Triggers, Grips, Jump)
    const auto& cfg = samu::config::Get();
    auto queryVec2 = [&](XrAction act, XrPath subPath, XrVector2f& outVec, bool& anyActive) {
        if (!act) return;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action        = act;
        gi.subactionPath = subPath;
        XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_session, &gi, &st)) && st.isActive) {
            outVec    = st.currentState;
            anyActive = true;
        }
    };
    auto queryFloat = [&](XrAction act, XrPath subPath, float& outVal, bool& anyActive) {
        if (!act) return;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action        = act;
        gi.subactionPath = subPath;
        XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
        if (XR_SUCCEEDED(xrGetActionStateFloat(m_session, &gi, &st)) && st.isActive) {
            outVal    = st.currentState;
            anyActive = true;
        }
    };
    auto queryBool = [&](XrAction act, XrPath subPath, bool& outVal, bool& anyActive) {
        if (!act) return;
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action        = act;
        gi.subactionPath = subPath;
        XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
        if (XR_SUCCEEDED(xrGetActionStateBoolean(m_session, &gi, &st)) && st.isActive) {
            if (st.currentState == XR_TRUE) outVal = true;
            anyActive = true;
        }
    };

    queryVec2(m_thumbstickAction,    m_handPaths[0], s.input.leftStick,    s.input.active);
    queryVec2(m_thumbstickAction,    m_handPaths[1], s.input.rightStick,   s.input.active);
    queryFloat(m_triggerValueAction, m_handPaths[0], s.input.leftTrigger,  s.input.active);
    queryFloat(m_triggerValueAction, m_handPaths[1], s.input.rightTrigger, s.input.active);
    queryFloat(m_squeezeValueAction, m_handPaths[0], s.input.leftGrip,     s.input.active);
    queryFloat(m_squeezeValueAction, m_handPaths[1], s.input.rightGrip,    s.input.active);
    queryBool(m_jumpButtonAction,    m_handPaths[0], s.input.jumpButtonPressed, s.input.active);
    queryBool(m_jumpButtonAction,    m_handPaths[1], s.input.jumpButtonPressed, s.input.active);
    queryBool(m_menuButtonAction,    m_handPaths[0], s.input.menuButtonPressed,   s.input.active);
    queryBool(m_menuButtonAction,    m_handPaths[1], s.input.menuButtonPressed,   s.input.active);
    queryBool(m_vrMenuButtonAction,  m_handPaths[1], s.input.vrMenuButtonPressed, s.input.active);
    queryBool(m_sprintClickAction,   m_handPaths[0], s.input.leftThumbstickClickPressed, s.input.active);

    s.input.rightTriggerPressed = (s.input.rightTrigger >= cfg.triggerThreshold);
    s.input.leftTriggerPressed  = (s.input.leftTrigger  >= cfg.triggerThreshold);
    s.input.rightGripPressed    = (s.input.rightGrip    >= cfg.gripThreshold);
    s.input.leftGripPressed     = (s.input.leftGrip     >= cfg.gripThreshold);

    // Locate stereo views (Left/Right eye poses + asymmetric frustum FOVs)
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime           = time;
    vli.space                 = m_baseSpace;

    XrView xrViews[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t viewCountOut = 0;
    if (XR_SUCCEEDED(xrLocateViews(m_session, &vli, &viewState, 2, &viewCountOut, xrViews)) &&
        viewCountOut == 2 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        s.viewsValid = true;
        for (int eye = 0; eye < 2; ++eye) {
            s.views[eye].pose       = xrViews[eye].pose;
            s.views[eye].fov        = xrViews[eye].fov;
            s.views[eye].projMatrix = math::ComposeAsymmetricProjectionLH(
                xrViews[eye].fov, cfg.nearClipUU, cfg.farClipUU);
            ClampedEyeExtent(m_viewConfigs[eye], s.views[eye].width, s.views[eye].height);
        }
        const float dx = xrViews[1].pose.position.x - xrViews[0].pose.position.x;
        const float dy = xrViews[1].pose.position.y - xrViews[0].pose.position.y;
        const float dz = xrViews[1].pose.position.z - xrViews[0].pose.position.z;
        s.ipdMeters = std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    PoseSample ps;
    ps.id        = s.sampleId;
    ps.head      = s.hmd.pose;
    ps.headValid = s.hmd.orientationValid;
    ps.eyesValid = s.viewsValid;
    if (s.viewsValid) {
        ps.eye[0] = s.views[0].pose;
        ps.eye[1] = s.views[1].pose;
    }

    std::lock_guard lock(m_stateMutex);
    m_tracking = s;
    m_poseHistory[ps.id % kPoseHistory] = ps;
}

TrackingState OpenXRManager::GetTrackingState() const {
    std::lock_guard lock(m_stateMutex);
    return m_tracking;
}

} // namespace vr
