// ScreenView::setupAndRender detour — screen-change publishing, the custom
// main-menu layout, vanilla chat-panel hiding and per-frame UI dispatch.
// a2 = MinecraftUIRenderContext* (confirmed in IDA ScreenView_setupAndRender).
#include "GameHooks.h"
#include "HookShared.h"
#include "../SDK/SDK.h"
#include "../SDK/Client/GameIcon.h"
#include "../Memory/OffsetManager.h"
#include "../Memory/OffsetValidator.h"
#include "../Modules/ModuleManager.h"
#include "../Modules/Visual/Chat.h"
#include "../Modules/Visual/HotbarAnimation.h"
#include "../Modules/Player/ChestStealer.h"
#include "../Modules/Player/InventoryManager.h"
#include "../Core/Logger.h"
#include "../Memory/SigManager.h"
#include "MinHook.h"
#include <cmath>
#include <string>

namespace {
    using namespace SDK;

    // Screen change tracking.
    std::string g_CurrentScreenName;

    // ---- start_screen main-menu layout -------------------------------------
    // stacked_rows children: play relY=0, settings=32, realms=64, store=96.
    // Move play+settings down; hide the realms/store rows so they don't leave
    // shadows on the moved buttons. Positive Y = lower.
    static constexpr float kMainMenuRowYOffset = 12.0f;
    static constexpr float kStackedRowsAbsX = 245.0f;
    static constexpr float kStackedRowsAbsY = 154.0f;

    static std::string sehReadControlName(SDK::UIControl* c) {
        std::string out;
        if (!c) return out;
        sdk_safe_string(c->getName(), out);
        return out;
    }

    // Recompute abs = parentAbs + parentRelative for a whole subtree.
    static void recomputeAbsTree(SDK::UIControl* c, float parentAbsX, float parentAbsY) {
        if (!c || !sdk_safe_ptr(c, 0x50)) return;
        const int flagsOff = Offsets().UI_Flags;
        const int childOff = Offsets().UI_Children;
        __try {
            Vec2* rel = c->getParentRelativePosition();
            Vec2* abs = c->getPosition();
            float ax = parentAbsX, ay = parentAbsY;
            if (rel && sdk_safe_ptr(rel, sizeof(Vec2))) {
                ax = parentAbsX + rel->x;
                ay = parentAbsY + rel->y;
            }
            if (abs && sdk_safe_ptr(abs, sizeof(Vec2))) {
                abs->x = ax;
                abs->y = ay;
            }
            if (flagsOff) *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(c) + flagsOff) |= 1ull;

            // children walk (shared_ptr vector)
            if (!childOff) return;
            uintptr_t base = reinterpret_cast<uintptr_t>(c) + childOff;
            if (!sdk_safe_ptr(reinterpret_cast<void*>(base), 0x18)) return;
            uintptr_t begin = *reinterpret_cast<uintptr_t*>(base);
            uintptr_t end   = *reinterpret_cast<uintptr_t*>(base + 8);
            if (!begin || end <= begin || ((end - begin) % 16) != 0) return;
            size_t n = (end - begin) / 16;
            if (n > 256) n = 256;
            for (size_t i = 0; i < n; ++i) {
                auto* child = *reinterpret_cast<SDK::UIControl**>(begin + i * 16);
                if (child && sdk_safe_ptr(child, 0x50))
                    recomputeAbsTree(child, ax, ay);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void sehHideControl(SDK::UIControl* c) {
        if (!c) return;
        const int flagsOff = Offsets().UI_Flags;
        __try {
            if (!sdk_safe_ptr(c, 0x50)) return;
            Vec2* sz  = c->getSize();
            Vec2* rel = c->getParentRelativePosition();
            Vec2* abs = c->getPosition();
            if (sz && sdk_safe_ptr(sz, sizeof(Vec2)))  { sz->x = 0.f; sz->y = 0.f; }
            if (rel && sdk_safe_ptr(rel, sizeof(Vec2))) { rel->y = -9999.f; }
            if (abs && sdk_safe_ptr(abs, sizeof(Vec2))) { abs->x = -9999.f; abs->y = -9999.f; }
            if (flagsOff) *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(c) + flagsOff) |= 1ull;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void hideTreeCb(SDK::UIControl* c, void* /*raw*/) {
        sehHideControl(c);
    }

    // Vanilla row Y under stacked_rows. <0 = not a move target.
    static float mainMenuMoveVanillaY(const std::string& n) {
        if (n == "play")     return 0.0f;
        if (n == "settings") return 32.0f;
        return -1.0f;
    }

    static bool isMainMenuHideRow(const std::string& n) {
        return n == "realms" || n == "store"
            || n == "realms_button" || n == "store_button"
            || n == "store_button_art" || n == "store_button_banner"
            || n == "main_buttons_panel_new_player_flow";
    }

    static void processMainMenuControlCb(SDK::UIControl* c, void* /*raw*/) {
        if (!c || !sdk_safe_ptr(c, 0x50)) return;
        const std::string n = sehReadControlName(c);

        if (isMainMenuHideRow(n)) {
            sehHideControl(c);
            c->forEachDescendantRaw(&hideTreeCb, nullptr);
            return;
        }

        const float vanilla = mainMenuMoveVanillaY(n);
        if (vanilla < 0.0f) return;

        Vec2* rel = c->getParentRelativePosition();
        Vec2* abs = c->getPosition();
        if (!rel || !sdk_safe_ptr(rel, sizeof(Vec2))) return;

        const float targetRel = vanilla + kMainMenuRowYOffset;
        const float targetAbs = kStackedRowsAbsY + targetRel;
        const bool relOk = std::fabs(rel->y - targetRel) < 0.25f;
        const bool absOk = abs && sdk_safe_ptr(abs, sizeof(Vec2)) &&
                           std::fabs(abs->y - targetAbs) < 0.25f;
        if (relOk && absOk) return;

        rel->y = targetRel;
        if (abs && sdk_safe_ptr(abs, sizeof(Vec2))) {
            abs->y = targetAbs;   // keep x
        }
        recomputeAbsTree(c, kStackedRowsAbsX, kStackedRowsAbsY);
    }

    static void processStartScreen(SDK::ScreenView* view) {
        if (!view) return;
        VisualTree* vt = view->getVisualTree();
        if (!vt || !sdk_safe_ptr(vt, 0x10)) return;
        UIControl* root = vt->getRootControl();
        if (!root || !sdk_safe_ptr(root, 0xA0)) return;
        root->forEachDescendantRaw(&processMainMenuControlCb, nullptr);
    }

    typedef void(__fastcall* ScreenView_setupAndRender_t)(SDK::ScreenView* view, void* mcuirc);
    ScreenView_setupAndRender_t oScreenViewSetupAndRender = nullptr;

    void __fastcall hkScreenViewSetupAndRender(SDK::ScreenView* view, void* mcuirc) {
        // One-shot offset probe: Present-time validation cannot reach the UI
        // graph, so this is the only place a live ScreenView is in hand.
        // Retries per frame until the report is final.
        OffsetValidator::GetInstance().ValidateScreenView(view);

        std::string name = view->getScreenName();
        SDK::ScreenController* ctrl = view ? view->getScreenController() : nullptr;
        SDK::ScreenView::currentScreenController() = ctrl;
        // Publish BEFORE the original render so text/image hooks see it.
        const bool trackedScreen =
            !name.empty() &&
            name.find("debug_screen") == std::string::npos &&
            name.find("toast_screen") == std::string::npos &&
            name.find("hud_crosshair_screen") == std::string::npos;
        if (trackedScreen) {
            SDK::ScreenView::currentScreenName() = name;
            if (g_CurrentScreenName != name) {
                g_CurrentScreenName = name;
                LOG_INFO("Screen changed: " + name, "GameHooks");
                SDK::ScreenEvents::get().fire(name);
            }

            // Hide the vanilla chat panel while the custom Chat module owns chat.
            // Re-applied every frame (the game can re-lay the panel anytime);
            // the restore also runs for one extra frame on the toggle edge.
            if (name == "hud_screen") {
                static bool s_vanillaChatHidden = false;
                Module* chatMod = ModuleManager::GetInstance().GetModuleByName("Chat");
                const bool wantHide = (chatMod && chatMod->IsEnabled());
                if (wantHide || wantHide != s_vanillaChatHidden) {
                    SDK::VisualTree* vt = view ? view->getVisualTree() : nullptr;
                    SDK::UIControl* root = vt ? vt->getRootControl() : nullptr;
                    SDK::UIControl* panel =
                        root ? SDK::findDescendantByName(root, "chat_panel") : nullptr;
                    const bool transitioned = (wantHide != s_vanillaChatHidden);
                    if (panel) {
                        panel->shiftOffscreen(wantHide);   // idempotent — safe per frame
                        if (transitioned) {
                            s_vanillaChatHidden = wantHide;
                            LOG_INFO(std::string("vanilla chat_panel ") +
                                     (wantHide ? "hidden" : "restored"), "GameHooks");
                        }
                    } else if (transitioned) {
                        s_vanillaChatHidden = wantHide;
                        LOG_WARN("chat_panel control not found on hud_screen", "GameHooks");
                    }
                }
            }

            // HotbarAnimation — per-frame hotbar selection-frame glide.
            SDK::VisualTree* hbVt = view ? view->getVisualTree() : nullptr;
            SDK::UIControl* hbRoot = hbVt ? hbVt->getRootControl() : nullptr;
            HotbarAnimation::OnScreenFrame(hbRoot);
        }

        const bool hooksReady = gh::HooksInstalled();
        const bool onStart = (name == "start_screen");

        if (hooksReady && onStart)
            processStartScreen(view);

        oScreenViewSetupAndRender(view, mcuirc);

        if (hooksReady && onStart)
            processStartScreen(view); // re-apply after the layout rebuild

        // ChestStealer primary path: UI thread + live controller. InventoryManager
        // uses the same dispatch. GameIcon::onUiRender harvests the game's own
        // item icons (the Present hook copies them into ImGui textures).
        if (hooksReady) {
            Module* cs = ModuleManager::GetInstance().GetModuleByName("ChestStealer");
            if (cs && cs->IsEnabled())
                static_cast<ChestStealer*>(cs)->OnContainerFrame(ctrl, name);

            Module* im = ModuleManager::GetInstance().GetModuleByName("InventoryManager");
            if (im && im->IsEnabled())
                static_cast<InventoryManager*>(im)->OnContainerFrame(ctrl, name);

            SDK::GameIcon::onUiRender(mcuirc, name);
        }
    }

    // Splash text: swap the string_view fed into the word-wrap pipeline.
    // Layout: { const char* data; size_t size; }
    typedef void* (__fastcall* SplashText_convert_t)(void* outLines, void* stringView, size_t maxWidth);
    static SplashText_convert_t oSplashText_convert = nullptr;

    static void* __fastcall hkSplashText_convert(void* outLines, void* /*stringView*/, size_t maxWidth) {
        static const char kSplash[] = "§zSunstead!";
        struct { const char* data; size_t size; } view{ kSplash, sizeof(kSplash) - 1 };
        if (oSplashText_convert)
            return oSplashText_convert(outLines, &view, maxWidth);
        return outLines;
    }
}

void InstallScreenHooks() {
    // Splash BEFORE ScreenView so the first frame already runs through it.
    uintptr_t convertAddr = SigManager::GetInstance().SplashText_convert;
    if (convertAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(convertAddr), &hkSplashText_convert,
                          reinterpret_cast<LPVOID*>(&oSplashText_convert)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(convertAddr));
            LOG_INFO("SplashText_convert hooked", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook SplashText_convert", "GameHooks");
        }
    } else {
        LOG_WARN("SplashText_convert signature not found; splash stays vanilla", "GameHooks");
    }

    // ScreenView must be the LAST hook — installed mid-init it raced the
    // installer and crashed.
    uintptr_t screenViewAddr = SigManager::GetInstance().ScreenView_setupAndRender;
    if (screenViewAddr) {
        if (MH_CreateHook(reinterpret_cast<LPVOID>(screenViewAddr), &hkScreenViewSetupAndRender,
                reinterpret_cast<LPVOID*>(&oScreenViewSetupAndRender)) == MH_OK) {
            gh::HookEnable(reinterpret_cast<LPVOID>(screenViewAddr));
            LOG_INFO("screen-render hook installed (last)", "GameHooks");
        } else {
            LOG_ERROR("FAILED to hook screen-render", "GameHooks");
        }
    } else {
        LOG_WARN("screen-render signature not found", "GameHooks");
    }
}
