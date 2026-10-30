#include "D2D.hpp"
#include "../../Core/Logger.h"
#include <winrt/base.h>
#include <stdio.h>
#include <dxgi1_3.h>
#include <unordered_map>
#include <cmath>
#include <algorithm>

struct BlurCallbackData;
static winrt::com_ptr<ID2D1Factory3> d2dFactory = nullptr;
static winrt::com_ptr<ID2D1Device> d2dDevice = nullptr;

static winrt::com_ptr<ID2D1Effect> blurEffect = nullptr;
static winrt::com_ptr<ID2D1Bitmap1> sourceBitmap = nullptr;

static winrt::com_ptr<ID2D1DeviceContext> d2dDeviceContext = nullptr;
static winrt::com_ptr<ID2D1SolidColorBrush> brush = nullptr;

bool initD2D = false;

// Frame-shared backbuffer snapshot for blur. The old per-region path did a
// CreateBitmap + CopyFromBitmap of EACH clip region every frame (ArrayList alone
// issues one per card), which stalled the GPU. Apollo's model copies the whole
// backbuffer ONCE per frame and lets every blur region sample from that single
// copy through a full-screen image brush. We adopt that here: the copy is made
// lazily on the first blur callback of a frame and released in endRender(). The
// per-region Flush() still stays (it is required for correct interleave with
// ImGui's D3D11 draws -- see the note in blurCallbackOptimized).
static ID2D1Bitmap* g_frameBlurCopy = nullptr;
static bool         g_frameBlurCopyValid = false;
// Pending optimized-blur callbacks this frame (batch Flush on the last one).
static int          g_blurFlushRemaining = 0;

static ID2D1Bitmap* ensureFrameBlurCopy() {
    if (g_frameBlurCopyValid && g_frameBlurCopy) return g_frameBlurCopy;
    if (!sourceBitmap) return nullptr;
    D2D1_SIZE_U size = sourceBitmap->GetPixelSize();
    if (g_frameBlurCopy) { g_frameBlurCopy->Release(); g_frameBlurCopy = nullptr; }
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(sourceBitmap->GetPixelFormat());
    if (FAILED(d2dDeviceContext->CreateBitmap(size, props, &g_frameBlurCopy)) || !g_frameBlurCopy)
        return nullptr;
    auto destPoint = D2D1::Point2U(0, 0);
    auto rect = D2D1::RectU(0, 0, size.width, size.height);
    g_frameBlurCopy->CopyFromBitmap(&destPoint, sourceBitmap.get(), &rect);
    g_frameBlurCopyValid = true;
    return g_frameBlurCopy;
}

// Persistent PRE-BLURRED full-screen bitmap. This is the real FPS fix: instead
// of letting every blur region re-evaluate the Gaussian effect graph over the
// whole backbuffer (each FillGeometry(imageBrush(effectOutput)) re-runs the
// blur -- N regions => N full-screen blurs => the frame-time sink the user saw,
// worst with ArrayList/ClickGUI columns issuing one blur per card), we run the
// Gaussian EXACTLY ONCE per (strength,mode) per frame, render it into this
// TARGET bitmap, and have every region sample this cheap static bitmap through
// an image brush. The bitmap object persists across frames (recreated only on
// resize); its CONTENTS are marked stale each frame via releaseFrameBlurCopy so
// the first blur of a new frame re-blurs the fresh backbuffer.
static ID2D1Bitmap1* g_blurredBitmap = nullptr;
static UINT  g_blurredW = 0, g_blurredH = 0;
static float g_blurredStrength = -1.0f; // -1 => contents stale, must re-blur
static int   g_blurredMode = -1;        // 0=QUALITY, 1=SPEED

static void releaseBlurredBitmap() {
    if (g_blurredBitmap) { g_blurredBitmap->Release(); g_blurredBitmap = nullptr; }
    g_blurredW = g_blurredH = 0;
    g_blurredStrength = -1.0f;
    g_blurredMode = -1;
}

static void releaseFrameBlurCopy() {
    if (g_frameBlurCopy) { g_frameBlurCopy->Release(); g_frameBlurCopy = nullptr; }
    g_frameBlurCopyValid = false;
    // Mark the pre-blurred contents stale (the KEEP the bitmap object). The next
    // frame's first blur callback re-runs the Gaussian against the fresh copy.
    g_blurredStrength = -1.0f;
    g_blurredMode = -1;
}

// Returns a full-screen bitmap holding the backbuffer blurred at `strength`.
// `speed` picks the cheaper Gaussian mode. Re-blurs only when the requested
// (strength,mode) differs from what is currently baked THIS frame; identical
// requests (the common case -- one global blur strength) reuse the bake, so the
// expensive Gaussian runs once no matter how many regions ask for it.
static ID2D1Bitmap1* ensureBlurredBitmap(float strength, bool speed) {
    if (!sourceBitmap || !d2dDeviceContext || !blurEffect) return nullptr;
    D2D1_SIZE_U size = sourceBitmap->GetPixelSize();
    if (g_blurredBitmap && (g_blurredW != size.width || g_blurredH != size.height))
        releaseBlurredBitmap();
    if (!g_blurredBitmap) {
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, sourceBitmap->GetPixelFormat());
        if (FAILED(d2dDeviceContext->CreateBitmap(size, nullptr, 0, props, &g_blurredBitmap)) ||
            !g_blurredBitmap)
            return nullptr;
        g_blurredW = size.width; g_blurredH = size.height;
        g_blurredStrength = -1.0f; g_blurredMode = -1;
    }
    const int wantMode = speed ? 1 : 0;
    if (g_blurredStrength == strength && g_blurredMode == wantMode)
        return g_blurredBitmap; // already baked this frame

    ID2D1Bitmap* frameCopy = ensureFrameBlurCopy();
    if (!frameCopy) return nullptr;
    blurEffect->SetInput(0, frameCopy);
    blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, strength);
    blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    blurEffect->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION,
                         speed ? D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED
                               : D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
    ID2D1Image* outImage = nullptr;
    blurEffect->GetOutput(&outImage);
    if (!outImage) return nullptr;
    d2dDeviceContext->SetTarget(g_blurredBitmap);
    d2dDeviceContext->DrawImage(outImage);
    d2dDeviceContext->SetTarget(sourceBitmap.get());
    outImage->Release();
    g_blurredStrength = strength;
    g_blurredMode = wantMode;
    return g_blurredBitmap;
}

static ID2D1Bitmap* cachedBitmap = nullptr;
static ID2D1ImageBrush* cachedBrush = nullptr;
static ID2D1RoundedRectangleGeometry* cachedClipRectGeo = nullptr;
static float cachedBlurStrength = -1.0f;
static bool requestFlush = false;

// blurCallbackOptimized uses the proven per-region blur mechanics (copy region
// -> Gaussian -> translated image brush -> FillGeometry) and DOES flush per
// region -- that flush is required for correctness because the callbacks run
// interleaved with ImGui's D3D11 draws on the same backbuffer (see the long
// note at the flush in blurCallbackOptimized). It differs from blurCallback
// only in using the cheaper SPEED Gaussian mode. releaseBakedBlurs is a
// harmless no-op kept so existing call sites (shutdown/endRender) stay valid.
static void releaseBakedBlurs() {}

float dpi = 0.0f;

void D2D::init(IDXGISwapChain* pSwapChain, ID3D11Device* pDevice) {
    if (initD2D) return;

    if (initD2D == false && SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.put()))) {
        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        pDevice->QueryInterface<IDXGIDevice>(dxgiDevice.put());
        if (dxgiDevice == nullptr) {
            return;
        }

        d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put());
        if (d2dDevice == nullptr) {
            return;
        }

        d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dDeviceContext.put());
        if (d2dDeviceContext == nullptr) {
            return;
        }

        d2dDeviceContext->CreateEffect(CLSID_D2D1GaussianBlur, blurEffect.put());
        if (blurEffect == nullptr) {
            return;
        }
    }

    initD2D = true;
    // store swapchain so callers can request blur even when GUI isn't open
    if (pSwapChain) {
        pSwapChain->AddRef();
        storedSwapChain = pSwapChain;
    }
}

void D2D::shutdown() {
    if (!initD2D) {
        return;
    }

    clearGhostFrames();
    releaseBakedBlurs();
    releaseFrameBlurCopy();
    releaseBlurredBitmap();
    d2dFactory = nullptr;
    d2dDevice = nullptr;
    d2dDeviceContext = nullptr;
    brush = nullptr;
    blurEffect = nullptr;
    sourceBitmap = nullptr;
    initD2D = false;
    LOG_INFO("Shutdown D2D", "D2D");
    if (storedSwapChain) {
        storedSwapChain->Release();
        storedSwapChain = nullptr;
    }
}

void D2D::beginRender(IDXGISurface* surface, float fxdpi) {
    if (!initD2D || isRenderingFrame) return;
    // If surface is null but we have a stored swapchain, try to acquire
    // its current backbuffer surface.
    if (surface == nullptr && storedSwapChain) {
        // Try to get the first backbuffer (works for typical DX11 swapchains).
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (SUCCEEDED(storedSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer))) {
            pBackBuffer->QueryInterface(__uuidof(IDXGISurface), (void**)&surface);
            pBackBuffer->Release();
        }
    }
    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );
    HRESULT hr = d2dDeviceContext->CreateBitmapFromDxgiSurface(surface, &bitmapProperties, sourceBitmap.put());

    if (FAILED(hr)) {
        LOG_ERROR("Failed to create bitmap from DXGI surface", "D2D");
        return;
    }

    d2dDeviceContext->SetTarget(sourceBitmap.get());
    d2dDeviceContext->BeginDraw();
    isRenderingFrame = true;
}

// Ghost bitmaps persist across frames; freed on resolution change / shutdown.
static std::vector<ID2D1Bitmap*> g_ghostBitmaps;
static UINT g_ghostW = 0, g_ghostH = 0;

void D2D::clearGhostFrames() {
    for (auto* b : g_ghostBitmaps) if (b) b->Release();
    g_ghostBitmaps.clear();
    g_ghostW = g_ghostH = 0;
}

void D2D::ghostFrameCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    auto data = (GhostCallbackData*)cmd->UserCallbackData;
    if (data == nullptr || !sourceBitmap) return;

    D2D1_SIZE_U px = sourceBitmap->GetPixelSize();
    D2D1_SIZE_F dip = sourceBitmap->GetSize();
    auto srcRect = D2D1::RectU(0, 0, px.width, px.height);
    auto destPoint = D2D1::Point2U(0, 0);
    D2D1_RECT_F dstF = D2D1::RectF(0.0f, 0.0f, dip.width, dip.height);

    if (px.width != g_ghostW || px.height != g_ghostH) {
        clearGhostFrames();
        g_ghostW = px.width; g_ghostH = px.height;
    }

    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(sourceBitmap->GetPixelFormat());

    int maxFrames = data->maxFrames;
    if (maxFrames < 1) maxFrames = 1;
    if (maxFrames > 20) maxFrames = 20;

    // Draw history FIRST (old frames over live image). Do NOT redraw the current
    // frame at alpha=1 — that made MotionBlur a no-op (identical full-screen copy).
    const float intensity = std::clamp(data->strength, 0.01f, 1.0f);
    const size_t n = g_ghostBitmaps.size();
    for (size_t i = 0; i < n; ++i) {
        ID2D1Bitmap* ghost = g_ghostBitmaps[i];
        if (!ghost) continue;
        // Older frames are earlier in the vector; fade by age from newest history.
        // age 1 = most recent past frame, age n = oldest.
        float age = (float)(n - i);
        float alpha = intensity * std::pow(0.72f, age - 1.0f);
        if (alpha <= 0.01f) continue;
        if (alpha > 0.95f) alpha = 0.95f;
        d2dDeviceContext->DrawBitmap(ghost, dstF, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    // Snapshot AFTER overlay so the trail compounds slightly (Latite pixel-average).
    while ((int)g_ghostBitmaps.size() >= maxFrames) {
        auto bitmap = g_ghostBitmaps.front();
        g_ghostBitmaps.erase(g_ghostBitmaps.begin());
        if (bitmap) bitmap->Release();
    }
    ID2D1Bitmap* ghostBitmap = nullptr;
    if (SUCCEEDED(d2dDeviceContext->CreateBitmap(px, props, &ghostBitmap)) && ghostBitmap) {
        ghostBitmap->CopyFromBitmap(&destPoint, sourceBitmap.get(), &srcRect);
        g_ghostBitmaps.push_back(ghostBitmap);
    }

    for (auto it = ghostCallbacks.begin(); it != ghostCallbacks.end(); it++) {
        if (it->get() == data) {
            ghostCallbacks.erase(it);
            break;
        }
    }
}

void D2D::addGhostFrame(ImDrawList* drawList, int maxFrames, float strength) {
    if (!initD2D) return;

    if (!isRenderingFrame && currentDxgiSurface) {
        beginRender(currentDxgiSurface, 96.0f);
    }
    if (!isRenderingFrame) return;

    auto uniqueData = std::make_shared<GhostCallbackData>(strength, maxFrames);
    auto data = uniqueData.get();
    ghostCallbacks.push_back(uniqueData);
    drawList->AddCallback(ghostFrameCallback, data);
}

void D2D::endRender() {
    if (!initD2D || !sourceBitmap) return;
    d2dDeviceContext->EndDraw();
    d2dDeviceContext->SetTarget(nullptr);
    sourceBitmap = nullptr;
    isRenderingFrame = false;

    // Free this frame's baked per-strength blur bitmaps/brushes.
    releaseBakedBlurs();
    // Invalidate the frame-shared blur snapshot so the next frame recopies the
    // fresh backbuffer.
    releaseFrameBlurCopy();
    g_blurFlushRemaining = 0;

    blurCallbacks.clear();
    ghostCallbacks.clear();

    if (cachedBitmap != nullptr) {
        cachedBitmap->Release();
        cachedBitmap = nullptr;
    }
    if (cachedBrush != nullptr) {
        cachedBrush->Release();
        cachedBrush = nullptr;
    }
    cachedBlurStrength = -1.0f;
    if (cachedClipRectGeo != nullptr) {
        cachedClipRectGeo->Release();
        cachedClipRectGeo = nullptr;
    }
}

void D2D::blurCallback(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    auto data = (BlurCallbackData*)cmd->UserCallbackData;
    if (data == nullptr) return;

    if (!sourceBitmap) return;
    ImVec4 clipRect = data->clipRect.has_value() ? *data->clipRect : cmd->ClipRect;

    float width = clipRect.z - clipRect.x;
    float height = clipRect.w - clipRect.y;
    if (width <= 0.0f || height <= 0.0f) return;

    // Sample from the ONCE-baked full-screen blurred bitmap (QUALITY mode).
    // The heavy Gaussian ran a single time in ensureBlurredBitmap; here we only
    // do a cheap image-brush fill of this region. The brush source rect spans
    // the whole screen and maps 1:1 to pixels (no transform), so FillGeometry
    // at clipRect samples the correct blurred area.
    ID2D1Bitmap1* blurred = ensureBlurredBitmap(data->strength, /*speed=*/false);
    if (!blurred) return;
    D2D1_SIZE_U bmpSize = sourceBitmap->GetPixelSize();
    D2D1_RECT_F screenRectF = D2D1::RectF(0.0f, 0.0f, (float)bmpSize.width, (float)bmpSize.height);
    D2D1_RECT_F clipRectD2D = D2D1::RectF(clipRect.x, clipRect.y, clipRect.z, clipRect.w);
    D2D1_ROUNDED_RECT clipRectRounded = D2D1::RoundedRect(clipRectD2D, data->rounding, data->rounding);

    ID2D1ImageBrush* outImageBrush = nullptr;
    D2D1_IMAGE_BRUSH_PROPERTIES outImage_props = D2D1::ImageBrushProperties(screenRectF);
    d2dDeviceContext->CreateImageBrush(blurred, outImage_props, &outImageBrush);

    if (outImageBrush) {
        ID2D1RoundedRectangleGeometry* clipRectGeo = nullptr;
        d2dFactory->CreateRoundedRectangleGeometry(clipRectRounded, &clipRectGeo);
        if (clipRectGeo) {
            d2dDeviceContext->FillGeometry(clipRectGeo, outImageBrush);
            clipRectGeo->Release();
        }
        outImageBrush->Release();
    }

    d2dDeviceContext->Flush();

    for (auto it = blurCallbacks.begin(); it != blurCallbacks.end(); it++) {
        if (it->get() == data) {
            blurCallbacks.erase(it);
            break;
        }
    }
}

bool D2D::addBlur(ImDrawList* drawList, float strength, std::optional<ImVec4> clipRect, float rounding) {
    if (!initD2D) return false;
    if (strength == 0) return false;

    if (!isRenderingFrame && currentDxgiSurface) {
        beginRender(currentDxgiSurface, 96.0f);
    }
    if (!isRenderingFrame) return false;

    auto uniqueData = std::make_shared<BlurCallbackData>(strength, rounding, clipRect);
    auto data = uniqueData.get();
    blurCallbacks.push_back(uniqueData);
    drawList->AddCallback(blurCallback, data);
    // D2D Flush mutates the D3D11 pipeline state. Tell ImGui to rebind its
    // shaders/RTV afterwards — without this, later draw commands can hang the
    // Present path and the whole HUD freezes until restart.
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

template <typename T>
void SafeRelease(T** ptr) {
    if (*ptr) {
        (*ptr)->Release();
        *ptr = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Optimized blur: samples the ONCE-baked full-screen Gaussian (SPEED mode).
// Flush is deferred to the last blur callback of the frame (see g_blurFlushRemaining).
// ---------------------------------------------------------------------------
void D2D::blurCallbackOptimized(const ImDrawList* parent_list, const ImDrawCmd* cmd) {
    auto data = (BlurCallbackData*)cmd->UserCallbackData;
    if (data == nullptr || !sourceBitmap) return;

    ImVec4 clipRect = data->clipRect.has_value() ? *data->clipRect : cmd->ClipRect;

    float width = clipRect.z - clipRect.x;
    float height = clipRect.w - clipRect.y;
    if (width <= 0.0f || height <= 0.0f) return;

    // Sample the ONCE-baked full-screen blurred bitmap (SPEED mode). The heavy
    // Gaussian ran a single time in ensureBlurredBitmap regardless of how many
    // regions (ArrayList cards, ClickGUI columns) request blur this frame --
    // that collapse from N full-screen blurs to 1 is the FPS fix. Here we only
    // do a cheap image-brush fill. The full-screen brush source rect maps 1:1
    // to screen pixels, so FillGeometry(clipRect) samples the right area.
    ID2D1Bitmap1* blurred = ensureBlurredBitmap(data->strength, /*speed=*/true);
    if (!blurred) return;
    D2D1_SIZE_U bmpSize = sourceBitmap->GetPixelSize();
    D2D1_RECT_F screenRectF = D2D1::RectF(0.0f, 0.0f, (float)bmpSize.width, (float)bmpSize.height);
    D2D1_RECT_F clipRectD2D = D2D1::RectF(clipRect.x, clipRect.y, clipRect.z, clipRect.w);
    D2D1_ROUNDED_RECT clipRectRounded = D2D1::RoundedRect(clipRectD2D, data->rounding, data->rounding);

    ID2D1ImageBrush* outImageBrush = nullptr;
    D2D1_IMAGE_BRUSH_PROPERTIES outImage_props = D2D1::ImageBrushProperties(screenRectF);
    d2dDeviceContext->CreateImageBrush(blurred, outImage_props, &outImageBrush);
    if (outImageBrush) {
        ID2D1RoundedRectangleGeometry* clipRectGeo = nullptr;
        d2dFactory->CreateRoundedRectangleGeometry(clipRectRounded, &clipRectGeo);
        if (clipRectGeo) {
            d2dDeviceContext->FillGeometry(clipRectGeo, outImageBrush);
            clipRectGeo->Release();
        }
        outImageBrush->Release();
    }

    // Flush EVERY blur region. The deferred "flush only on the last callback"
    // batching was a frame-ordering bug: the batched D2D fills stayed pending
    // while ImGui went on drawing the panels and TEXT of the modules between
    // blur regions, and the final flush then painted the pending blurred fills
    // ON TOP of already-drawn text (the Coordinates/FPS/Ping "text disappears
    // with blur" bug). HUD frames issue a handful of blur regions, so a
    // per-region GPU sync costs nothing and keeps the draw order strictly
    // blur -> panel -> text for every module.
    d2dDeviceContext->Flush();
    g_blurFlushRemaining = 0;
}

bool D2D::addBlurOptimized(ImDrawList* drawList, float strength, std::optional<ImVec4> clipRect, float rounding) {
    if (!initD2D) return false;
    // Treat near-zero as off — avoids Gaussian setup for "0" slider noise.
    if (strength < 0.05f) return false;

    if (!isRenderingFrame && currentDxgiSurface) {
        beginRender(currentDxgiSurface, 96.0f);
    }
    if (!isRenderingFrame) return false;

    // Quantize strength so near-identical values share one bake.
    strength = std::round(strength * 2.0f) * 0.5f;
    if (strength < 0.05f) return false;

    auto uniqueData = std::make_shared<BlurCallbackData>(strength, rounding, clipRect);
    auto data = uniqueData.get();
    blurCallbacks.push_back(uniqueData);
    ++g_blurFlushRemaining;
    drawList->AddCallback(blurCallbackOptimized, data);
    // Reset ImGui state only after the last pending blur for this batch window.
    // Always schedule reset — ImGui needs it after any D2D flush that may run.
    drawList->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}