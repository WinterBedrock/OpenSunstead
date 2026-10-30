#pragma once
#include <Windows.h>
#include <string>
#include <cmath>
#include <imgui.h>
// For the deliberate-probe markers used by sdk_safe_ptr below. Header-only and
// dependency-free, so this does not drag the logger into every SDK translation
// unit.
#include "../Core/CrashHandler.h"

struct Vec2 { float x,y; Vec2():x(0),y(0){} Vec2(float x,float y):x(x),y(y){} };
struct Vec3 { float x,y,z; Vec3():x(0),y(0),z(0){} Vec3(float x,float y,float z):x(x),y(y),z(z){} Vec3 operator+(const Vec3&o)const{return Vec3(x+o.x,y+o.y,z+o.z);} Vec3 operator-(const Vec3&o)const{return Vec3(x-o.x,y-o.y,z-o.z);} };
struct Color { float r,g,b,a; Color():r(0),g(0),b(0),a(0){} Color(float r,float g,float b,float a):r(r),g(g),b(b),a(a){} };
struct AABB { Vec3 lower,higher; void rebase(const Vec3&p){Vec3 d=higher-lower;lower.x=p.x-d.x/2.f;lower.y=p.y;lower.z=p.z-d.z/2.f;higher.x=p.x+d.x/2.f;higher.y=p.y+d.y;higher.z=p.z+d.z/2.f;} };
struct StateVectorComponent { Vec3 pos,posOld,velocity; };
struct AABBShapeComponent { Vec3 mMin,mMax; float width,height; };
// Full 0x10 layout: mPitch, mYaw, mOldPitch, mOldYaw. The game interpolates the
// rendered rotation between the old/current pair, so leaving `rotationOld` stale
// lands the effective yaw halfway ("crooked" aim). Aim code must set both.
struct ActorRotationComponent { Vec2 rotation; Vec2 rotationOld; };
struct ActorHeadRotationComponent { float mHeadRot; float mOldHeadRot; };
struct MobBodyRotationComponent   { float yBodyRot; float yOldBodyRot; };
struct Matrix4 { float m[16]; };

// Is `ptr` readable for `sz` bytes? Answered by reading it and catching the
// fault — an AV here IS the answer "no", which happens routinely for fields the
// game has not populated yet. Probe markers let the first-chance vectored
// exception handler tell a healthy probe from a real crash. The __try body
// stays POD-only (C2712): explicit enter/leave instead of RAII.
inline bool sdk_safe_ptr(void* ptr, size_t sz) {
    if(!ptr) return false; uintptr_t a=(uintptr_t)ptr;
    if(a<0x10000||a>=0x00007FFFFFFFFFFFLL) return false;
    CrashHandler::EnterProbe();
    __try{volatile char c=*(volatile char*)ptr; c=*(volatile char*)(a+sz-1);
          CrashHandler::LeaveProbe(); return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){CrashHandler::LeaveProbe(); return false;}
}
// Copy a std::string that MAY NOT BE ONE into a fixed buffer — fully
// SEH-guarded and POD-only. `out = *s` is unsafe even when length/data probe
// OK: operator= re-reads size/capacity and can allocate from bogus fields,
// faulting mid-assignment. Copying through a char buffer keeps the guarded
// region POD-only (C2712).
inline bool sdk_copy_cstr(void* ptr, char* buf, size_t bufSize, size_t* outLen) {
    if (!ptr || !buf || bufSize == 0) return false;
    CrashHandler::EnterProbe();
    bool ok = false;
    __try {
        auto* s = (std::string*)ptr;
        const size_t len = s->length();
        if (len < bufSize) {
            const char* d = s->c_str();
            if (d) {
                for (size_t i = 0; i < len; ++i) buf[i] = d[i];
                buf[len] = '\0';
                *outLen = len;
                ok = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    CrashHandler::LeaveProbe();
    return ok;
}

inline bool sdk_safe_string(void* ptr, std::string& out) {
    if (!sdk_safe_ptr(ptr, sizeof(std::string))) return false;
    char buf[257];
    size_t len = 0;
    if (!sdk_copy_cstr(ptr, buf, sizeof(buf), &len)) return false;
    // Assigning to `out` needs unwind state, so it happens here, outside any
    // __try — and from OUR buffer, which is known-good.
    out.assign(buf, len);
    return true;
}

// Shared vector / angle / matrix / worldToScreen math over the geometry PODs
// declared above. Included last so Vec2/Vec3/Matrix4/AABB are complete.
#include "Math.h"
