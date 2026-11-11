#pragma once
#include <Windows.h>
#include <string>
#include <cmath>
#include <imgui.h>
#include "../Core/CrashHandler.h"

struct Vec2 { float x,y; Vec2():x(0),y(0){} Vec2(float x,float y):x(x),y(y){} };
struct Vec3 { float x,y,z; Vec3():x(0),y(0),z(0){} Vec3(float x,float y,float z):x(x),y(y),z(z){} Vec3 operator+(const Vec3&o)const{return Vec3(x+o.x,y+o.y,z+o.z);} Vec3 operator-(const Vec3&o)const{return Vec3(x-o.x,y-o.y,z-o.z);} };
struct Color { float r,g,b,a; Color():r(0),g(0),b(0),a(0){} Color(float r,float g,float b,float a):r(r),g(g),b(b),a(a){} };
struct AABB { Vec3 lower,higher; void rebase(const Vec3&p){Vec3 d=higher-lower;lower.x=p.x-d.x/2.f;lower.y=p.y;lower.z=p.z-d.z/2.f;higher.x=p.x+d.x/2.f;higher.y=p.y+d.y;higher.z=p.z+d.z/2.f;} };
struct StateVectorComponent { Vec3 pos,posOld,velocity; };
struct AABBShapeComponent { Vec3 mMin,mMax; float width,height; };
struct ActorRotationComponent { Vec2 rotation; Vec2 rotationOld; };
struct ActorHeadRotationComponent { float mHeadRot; float mOldHeadRot; };
struct MobBodyRotationComponent   { float yBodyRot; float yOldBodyRot; };
struct Matrix4 { float m[16]; };

inline bool sdk_safe_ptr(void* ptr, size_t sz) {
    if(!ptr) return false; uintptr_t a=(uintptr_t)ptr;
    if(a<0x10000||a>=0x00007FFFFFFFFFFFLL) return false;
    CrashHandler::EnterProbe();
    __try{volatile char c=*(volatile char*)ptr; c=*(volatile char*)(a+sz-1);
          CrashHandler::LeaveProbe(); return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){CrashHandler::LeaveProbe(); return false;}
}
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
    out.assign(buf, len);
    return true;
}

// Shared vector / angle / matrix / worldToScreen math over the geometry PODs
// declared above. Included last so Vec2/Vec3/Matrix4/AABB are complete.
#include "Math.h"
