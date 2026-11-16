#pragma once
// src/vr_core/XrMath.h
// OpenXR math utilities:
// - Right-handed Y-up (OpenXR meters) <-> Left-handed Z-up (UE3 Unreal Units & FRotator 65536=360deg)
// - Asymmetric frustum projection matrices for DirectX 9 / UE3 stereo rendering
// Completely decoupled from src/ue3_bridge/.

#include <openxr/openxr.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace vr::math {

inline constexpr float kPi         = 3.14159265358979323846f;
inline constexpr float kRad2Deg    = 57.2957795130823208768f;
inline constexpr float kDeg2Rad    = 0.01745329251994329577f;
// Unreal Engine 3 rotation units: 65536 = 360 degrees = 2*pi radians
inline constexpr float kRad2UnrRot = 10430.3783504704527f; // 32768.0 / pi
inline constexpr float kDeg2UnrRot = 182.04444444444445f;  // 65536.0 / 360.0
inline constexpr float kUnrRot2Rad = 0.00009587379924285f; // pi / 32768.0

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RotatorI32 {
    int32_t pitch = 0;
    int32_t yaw   = 0;
    int32_t roll  = 0;
};

// Row-major 4x4 matrix (matches D3DMATRIX and UE3 FMatrix memory layout).
struct Mat4 {
    float m[4][4]{};

    static Mat4 Identity() {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

inline XrPosef IdentityPose() {
    XrPosef p{};
    p.orientation.w = 1.0f;
    return p;
}

inline XrQuaternionf Normalize(XrQuaternionf q) {
    float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len <= 1e-8f) return {0, 0, 0, 1};
    return {q.x / len, q.y / len, q.z / len, q.w / len};
}

// Rotates a 3D vector in OpenXR space by an OpenXR unit quaternion.
inline XrVector3f RotateVector(XrQuaternionf q, XrVector3f v) {
    // t = 2 * cross(q.xyz, v)
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);
    // v' = v + q.w * t + cross(q.xyz, t)
    return {
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx)
    };
}

// Yaw/Pitch/Roll in degrees from an OpenXR (right-handed, Y-up, -Z forward) quaternion.
struct EulerDeg { float yaw, pitch, roll; };
inline EulerDeg ToEulerDeg(XrQuaternionf q) {
    q = Normalize(q);
    float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    sinp = sinp > 1.f ? 1.f : (sinp < -1.f ? -1.f : sinp);
    EulerDeg e;
    e.pitch = std::asin(sinp) * kRad2Deg;
    e.yaw   = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)) * kRad2Deg;
    e.roll  = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.x * q.x + q.z * q.z)) * kRad2Deg;
    return e;
}

// ---------------------------------------------------------------------------
// OpenXR (Right-Handed, +X Right, +Y Up, -Z Forward, Meters)
//   -> UE3 (Left-Handed, +X Forward, +Y Right, +Z Up, Unreal Units)
// Default scale: 1.0 m = 50.0 UU (configurable via samu_vr.ini WorldScale).
// ---------------------------------------------------------------------------
inline Vec3 XrToUe3Position(XrVector3f xrPos, float worldScale = 50.0f) {
    return {
        -xrPos.z * worldScale, // +X_ue = Forward (-Z_xr)
        +xrPos.x * worldScale, // +Y_ue = Right   (+X_xr)
        +xrPos.y * worldScale  // +Z_ue = Up      (+Y_xr)
    };
}

// Rotates a local UE3 roomscale offset (X forward, Y right, Z up) by a base UE3 yaw (in URU).
inline Vec3 RotateUe3OffsetByYaw(Vec3 localUe, int32_t baseYawUru) {
    float yawRad = static_cast<float>(baseYawUru) * kUnrRot2Rad;
    float c = std::cos(yawRad);
    float s = std::sin(yawRad);
    return {
        localUe.x * c - localUe.y * s,
        localUe.x * s + localUe.y * c,
        localUe.z
    };
}

// Converts an OpenXR orientation quaternion into UE3 FRotator (Pitch, Yaw, Roll in 65536=360deg units).
// Uses exact basis-vector mapping into UE3 left-handed space (+X Forward, +Y Right, +Z Up):
//   Pitch: nose up (+), nose down (-)
//   Yaw:   turn right (+), turn left (-)
//   Roll:  tilt right ear down (+), left ear down (-)
inline RotatorI32 XrToUe3Rotator(XrQuaternionf q) {
    q = Normalize(q);
    // Transform OpenXR camera basis vectors: Forward=(0,0,-1), Right=(1,0,0), Up=(0,1,0)
    XrVector3f xrFwd   = RotateVector(q, {0.0f, 0.0f, -1.0f});
    XrVector3f xrRight = RotateVector(q, {1.0f, 0.0f,  0.0f});
    XrVector3f xrUp    = RotateVector(q, {0.0f, 1.0f,  0.0f});

    // Map into UE3 coordinates: X = -xr.z, Y = +xr.x, Z = +xr.y
    Vec3 fwd   = {-xrFwd.z,   xrFwd.x,   xrFwd.y};
    Vec3 right = {-xrRight.z, xrRight.x, xrRight.y};
    Vec3 up    = {-xrUp.z,    xrUp.x,    xrUp.y};

    float pitchRad = std::atan2(fwd.z, std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y));
    float yawRad   = std::atan2(fwd.y, fwd.x);
    // Project right & up onto the plane perpendicular to fwd to extract roll cleanly:
    float sinYaw = std::sin(yawRad);
    float cosYaw = std::cos(yawRad);
    // Horizontal right reference in UE3 XY plane: (-sinYaw, cosYaw, 0)
    float rollY = -(right.x * (-sinYaw) + right.y * cosYaw);
    float rollX =  (up.x    * (-sinYaw) + up.y    * cosYaw);
    float rollRad = std::atan2(right.z, right.x * (-sinYaw) + right.y * cosYaw);
    (void)rollY; (void)rollX;
    // Note: in UE3, positive Roll tilts the right side (+Y) down (-Z), so right.z < 0 -> Roll > 0:
    rollRad = -rollRad;
    // FIX: the expression above is only correct at zero pitch (it yields atan(tan(roll) * cos(pitch))).
    // In UE3's FRotationMatrix the right axis has Z = -sin(roll)*cos(pitch) and the up axis has
    // Z = cos(roll)*cos(pitch), so the exact roll is:
    rollRad = std::atan2(-right.z, up.z);

    RotatorI32 rot;
    rot.pitch = static_cast<int32_t>( std::lround(pitchRad * kRad2UnrRot)) & 0xFFFF;
    rot.yaw   = static_cast<int32_t>( std::lround(yawRad   * kRad2UnrRot)) & 0xFFFF;
    rot.roll  = static_cast<int32_t>( std::lround(rollRad  * kRad2UnrRot)) & 0xFFFF;
    // Normalize signed 16-bit range [-32768, 32767] for arithmetic convenience
    if (rot.pitch > 32767) rot.pitch -= 65536;
    if (rot.yaw   > 32767) rot.yaw   -= 65536;
    if (rot.roll  > 32767) rot.roll  -= 65536;
    return rot;
}

// Converts an OpenXR orientation quaternion directly into a UE3 forward unit vector (+X Forward, +Y Right, +Z Up).
inline Vec3 XrToUe3Forward(XrQuaternionf q) {
    q = Normalize(q);
    XrVector3f xrFwd = RotateVector(q, {0.0f, 0.0f, -1.0f});
    Vec3 fwd = {-xrFwd.z, xrFwd.x, xrFwd.y};
    float len = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
    if (len <= 1e-6f) return {1.0f, 0.0f, 0.0f};
    return {fwd.x / len, fwd.y / len, fwd.z / len};
}

// Converts a UE3 FRotator (Pitch, Yaw, Roll in 65536=360deg units) into a UE3 forward unit vector.
inline Vec3 Ue3RotatorToForward(RotatorI32 rot) {
    float pitchRad = static_cast<float>(rot.pitch) * kUnrRot2Rad;
    float yawRad   = static_cast<float>(rot.yaw)   * kUnrRot2Rad;
    float cp = std::cos(pitchRad);
    float sp = std::sin(pitchRad);
    float cy = std::cos(yawRad);
    float sy = std::sin(yawRad);
    return {cp * cy, cp * sy, sp};
}

// ---------------------------------------------------------------------------
// OpenXR Asymmetric Frustum Projection Matrix (DirectX 9 / UE3 Left-Handed Z in [0, 1])
// ---------------------------------------------------------------------------
inline Mat4 ComposeAsymmetricProjectionLH(const XrFovf& fov, float nearZ, float farZ) {
    const float tanLeft  = std::tan(fov.angleLeft);
    const float tanRight = std::tan(fov.angleRight);
    const float tanDown  = std::tan(fov.angleDown);
    const float tanUp    = std::tan(fov.angleUp);

    const float tanWidth  = tanRight - tanLeft;
    const float tanHeight = tanUp - tanDown;

    Mat4 p{};
    if (tanWidth <= 1e-6f || tanHeight <= 1e-6f || farZ <= nearZ) {
        return Mat4::Identity();
    }

    // Row-major left-handed perspective off-center matrix (v_clip = v_view * P, Z in [0, 1]):
    //   m[0][0] = 2 / (tanRight - tanLeft)
    //   m[1][1] = 2 / (tanUp - tanDown)
    //   m[2][0] = -(tanRight + tanLeft) / (tanRight - tanLeft)
    //   m[2][1] = -(tanUp + tanDown) / (tanUp - tanDown)
    //   m[2][2] = farZ / (farZ - nearZ)
    //   m[2][3] = 1.0f
    //   m[3][2] = -(farZ * nearZ) / (farZ - nearZ)
    p.m[0][0] =  2.0f / tanWidth;
    p.m[1][1] =  2.0f / tanHeight;
    p.m[2][0] = -(tanRight + tanLeft) / tanWidth;
    p.m[2][1] = -(tanUp + tanDown) / tanHeight;
    p.m[2][2] =  farZ / (farZ - nearZ);
    p.m[2][3] =  1.0f;
    p.m[3][2] = -(farZ * nearZ) / (farZ - nearZ);
    return p;
}

inline float FovHorizontalDeg(const XrFovf& fov) {
    return (fov.angleRight - fov.angleLeft) * kRad2Deg;
}

inline float FovVerticalDeg(const XrFovf& fov) {
    return (fov.angleUp - fov.angleDown) * kRad2Deg;
}

} // namespace vr::math
