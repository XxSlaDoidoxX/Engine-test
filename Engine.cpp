#include "pch.h" 

using namespace winrt;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::System;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Composition::Desktop;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;


// ============================= DWM PRIVATE DEFINITIONS & COM INTEROP ============================= \\
// Credits for uncovering private APIs: https://blog.adeltax.com/dwm-thumbnails-but-with-idcompositionvisual/ | https://gist.github.com/ADeltaX/c0e565a50d2cedb62dab5c13674cb8b5

#define DWM_TNP_ENABLE3D          0x4000000

typedef HRESULT(WINAPI* DwmpCreateSharedThumbnailVisual)(
    HWND hwndDestination, HWND hwndSource, DWORD dwThumbnailFlags,
    DWM_THUMBNAIL_PROPERTIES* pThumbnailProperties, VOID* pDCompDevice,
    VOID** ppVisual, PHTHUMBNAIL phThumbnailId);

typedef HRESULT(WINAPI* DwmpQueryWindowThumbnailSourceSize)(
    HWND hwndSource, BOOL fSourceClientAreaOnly, SIZE* pSize);

static DwmpCreateSharedThumbnailVisual g_DwmpCreateSharedThumbnailVisual = nullptr;
static DwmpQueryWindowThumbnailSourceSize g_DwmpQueryWindowThumbnailSourceSize = nullptr;

// Undocumented Interop Interfaces \\
#undef INTERFACE
#define INTERFACE IInteropCompositorPartnerCallback
DECLARE_INTERFACE_IID_(IInteropCompositorPartnerCallback, IUnknown, "9bb59fc9-3326-4c32-bf06-d6b415ac2bc5")
{
    STDMETHOD(NotifyDirty)(THIS_) PURE;
    STDMETHOD(NotifyDeferralState)(THIS_ bool deferRequested) PURE;
};

#undef INTERFACE
#define INTERFACE IInteropCompositorFactoryPartner
DECLARE_INTERFACE_IID_(IInteropCompositorFactoryPartner, IInspectable, "22118adf-23f1-4801-bcfa-66cbf48cc51b")
{
    STDMETHOD(CreateInteropCompositor)(THIS_ IUnknown * renderingDevice, IInteropCompositorPartnerCallback * callback, REFIID iid, VOID * *instance) PURE;
    STDMETHOD(CheckEnabled)(THIS_ bool* enableInteropCompositor, bool* enableExposeVisual) PURE;
};

#undef INTERFACE
#define INTERFACE IInteropCompositorPartner
DECLARE_INTERFACE_IID_(IInteropCompositorPartner, IUnknown, "e7894c70-af56-4f52-b382-4b3cd263dc6f")
{
    STDMETHOD(MarkDirty)(THIS_) PURE;
    STDMETHOD(ClearCallback)(THIS_) PURE;
    STDMETHOD(CreateManipulationTransform)(THIS_ IDCompositionTransform * transform, REFIID iid, VOID * *result) PURE;
    STDMETHOD(RealClose)(THIS_) PURE;
};

// ============================= GLOBAL PRELOADED STATE ============================= \\

DWORD g_compositorThreadId = 0;
std::atomic<bool> g_isCompositorReady{ false };
std::atomic<bool> g_isShuttingDown{ false };
std::atomic<bool> g_isBorderlessGranted{ false };
std::thread g_compThread;

DispatcherQueueController g_queueController{ nullptr };
Compositor g_compositor{ nullptr };

static com_ptr<ID3D11Device> g_d3dDevice;
static com_ptr<ABI::Windows::UI::Composition::ICompositionGraphicsDevice> g_compGraphics;
static IDirect3DDevice g_winrtDevice{ nullptr };
static std::mutex g_d3dMutex;

static com_ptr<IDCompositionDesktopDevice> g_dcompDevice;
static HWND g_dwmOwnerHwnd = nullptr;

// Fallback from the private API to WGC logic in the future -> useless for now
struct WgcCaptureState {
    HWND targetHwnd{ nullptr };
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool framePool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    CompositionDrawingSurface drawingSurface{ nullptr };
    SIZE currentSize{ 0, 0 };
    std::atomic<bool> isClosing{ false };
    std::atomic<int> frameCount{ 0 };
};

struct DwmCaptureState {
    HWND targetHwnd{ nullptr };
    HWND ghostHwnd{ nullptr };
    HTHUMBNAIL hThumb{ nullptr };

    DesktopWindowTarget compTarget{ nullptr };
    ContainerVisual rootVisual{ nullptr };
    com_ptr<IDCompositionVisual2> rawVisual;
    Visual compVisual{ nullptr };
    SIZE currentSize{ 0, 0 };
    std::atomic<bool> isClosing{ false };
};

static std::mutex g_captureMapMutex;
static std::unordered_map<HWND, WgcCaptureState*> g_activeWgcCaptures;
static std::unordered_map<HWND, DwmCaptureState*> g_activeDwmCaptures;

// ============================= HELPERS ============================= \\

static std::mutex g_logMutex;

void EngineLog(const char* format, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* file;
    if (fopen_s(&file, "C:\\TWM\\engine_log.txt", "a") == 0) {
        va_list args;
        va_start(args, format);
        vfprintf(file, format, args);
        va_end(args);
        fclose(file);
    }
}

RECT GetTrueWindowRect(HWND hWnd) {
    RECT rect = { 0 };
    if (SUCCEEDED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) {
        return rect;
    }
    GetWindowRect(hWnd, &rect);
    return rect;
}

DesktopWindowTarget CreateDesktopWindowTarget(Compositor const& compositor, HWND window) {
    namespace abi = ABI::Windows::UI::Composition::Desktop;
    auto interop = compositor.as<abi::ICompositorDesktopInterop>();
    DesktopWindowTarget target{ nullptr };
    check_hresult(interop->CreateDesktopWindowTarget(
        window, true, reinterpret_cast<abi::IDesktopWindowTarget**>(put_abi(target))
    ));
    return target;
}

Visual DCompVisualToVisual(Compositor compositor, IDCompositionVisual2* dcompVisual, SIZE dcompVisualSize) {
    winrt::com_ptr<IDCompositionVisual2> dcompVisualContainer;
    g_dcompDevice->CreateVisual(dcompVisualContainer.put());
    dcompVisualContainer->AddVisual(dcompVisual, TRUE, nullptr);
    auto visualContainer = dcompVisualContainer.as<Visual>();
    visualContainer.Size({ (float)dcompVisualSize.cx, (float)dcompVisualSize.cy });
    return visualContainer;
}

// ============================= BACKGROUND THREAD (INIT) ============================= \\
void CompositorThread() {
    EngineLog("[INIT] CompositorThread started. ThreadID: %lu\n", GetCurrentThreadId());
    g_compositorThreadId = GetCurrentThreadId();
    init_apartment(apartment_type::single_threaded);

    try {
        auto asyncOp = GraphicsCaptureAccess::RequestAccessAsync(GraphicsCaptureAccessKind::Borderless);
        asyncOp.Completed([](auto const&, winrt::Windows::Foundation::AsyncStatus status) {
            g_isBorderlessGranted = true;
            });
    }
    catch (...) { g_isBorderlessGranted = true; }

    HMODULE hDwmApi = LoadLibraryA("dwmapi.dll");
    if (hDwmApi) {
        g_DwmpCreateSharedThumbnailVisual = (DwmpCreateSharedThumbnailVisual)GetProcAddress(hDwmApi, MAKEINTRESOURCEA(147));
        g_DwmpQueryWindowThumbnailSourceSize = (DwmpQueryWindowThumbnailSourceSize)GetProcAddress(hDwmApi, MAKEINTRESOURCEA(162));
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "GhostCaptureClass";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_ASTA };
    ABI::Windows::System::IDispatcherQueueController* ptrController{ nullptr };
    check_hresult(CreateDispatcherQueueController(options, &ptrController));
    g_queueController = DispatcherQueueController{ ptrController, take_ownership_from_abi };

    try {
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr, 0, D3D11_SDK_VERSION, g_d3dDevice.put(), nullptr, nullptr);

        com_ptr<IDXGIDevice> dxgiDevice = g_d3dDevice.as<IDXGIDevice>();
        com_ptr<IInspectable> winrtDeviceIns;
        CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), winrtDeviceIns.put());
        g_winrtDevice = winrtDeviceIns.as<IDirect3DDevice>();

        com_ptr<ID2D1Factory1> d2dFactory;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), d2dFactory.put_void());

        com_ptr<ID2D1Device> d2dDevice;
        d2dFactory->CreateDevice(dxgiDevice.get(), d2dDevice.put());

        com_ptr<IInteropCompositorFactoryPartner> interopFactory;
        HSTRING className = nullptr;
        if (SUCCEEDED(WindowsCreateString(L"Windows.UI.Composition.Compositor", 33, &className))) {
            RoGetActivationFactory(className, __uuidof(IInteropCompositorFactoryPartner), interopFactory.put_void());
            WindowsDeleteString(className);
        }

        if (interopFactory) {
            com_ptr<IInteropCompositorPartner> interopCompositor;
            HRESULT hrInterop = interopFactory->CreateInteropCompositor(
                d2dDevice.get(), nullptr, __uuidof(IInteropCompositorPartner), interopCompositor.put_void());

            if (SUCCEEDED(hrInterop)) {
                g_compositor = interopCompositor.as<Compositor>();
                g_dcompDevice = interopCompositor.as<IDCompositionDesktopDevice>();
                EngineLog("[INIT] Interop Compositor and DComp Device successfully initialized.\n");
            }
        }

        com_ptr<ABI::Windows::UI::Composition::ICompositorInterop> compInterop =
            g_compositor.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        compInterop->CreateGraphicsDevice(d2dDevice.get(), g_compGraphics.put());

        g_isCompositorReady = true;
    }
    catch (...) {}

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        FILE* clearFile;
        if (fopen_s(&clearFile, "C:\\TWM\\engine_log.txt", "w") == 0) fclose(clearFile);

        g_isShuttingDown = false;
        g_compThread = std::thread(CompositorThread);
    }
    return TRUE;
}
// ============================= DWM EXPORTED FUNCTIONS ============================= \\

extern "C" __declspec(dllexport) bool TryStartDwmCaptureTest(HWND targetHwnd) {
    if (g_isShuttingDown || !g_queueController || !targetHwnd || !g_DwmpCreateSharedThumbnailVisual) return false;

    {
        std::lock_guard<std::mutex> lock(g_captureMapMutex);
        if (g_activeDwmCaptures.find(targetHwnd) != g_activeDwmCaptures.end()) return true;
    }

    RECT r = GetTrueWindowRect(targetHwnd);
    SIZE expectedSize = { r.right - r.left, r.bottom - r.top };

    // Wait for DWM to have a true texture (block 250x141 icons) - doesn't seem to work
    SIZE windowSize{};
    if (g_DwmpQueryWindowThumbnailSourceSize) {
        HRESULT hr = g_DwmpQueryWindowThumbnailSourceSize(targetHwnd, FALSE, &windowSize);
        if (FAILED(hr)) return false;
    }

    // This works to return a true texture but seems a bit workaroundish
    if (std::abs(windowSize.cx - expectedSize.cx) > 15 ||
        std::abs(windowSize.cy - expectedSize.cy) > 15) {
        return false; // Return false so Windhawk polling loop keeps waiting
    }

    auto queue = g_queueController.DispatcherQueue();
    queue.TryEnqueue([=]() {
        if (g_isShuttingDown) return;

        std::lock_guard<std::mutex> lock(g_captureMapMutex);
        if (g_activeDwmCaptures.find(targetHwnd) != g_activeDwmCaptures.end()) return;

        try {
            // Create the hidden ghost window for preloading
            HWND ghostHwnd = CreateWindowExA(
                WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                "GhostCaptureClass", "Ghost", WS_POPUP,
                0, 0, 1, 1,
                nullptr, nullptr, GetModuleHandle(nullptr), nullptr
            );

            if (!ghostHwnd) return;
            SetLayeredWindowAttributes(ghostHwnd, 0, 255, LWA_ALPHA);

            DWM_THUMBNAIL_PROPERTIES thumb{};
            thumb.dwFlags = DWM_TNP_VISIBLE | DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_ENABLE3D;
            thumb.opacity = 255;
            thumb.fVisible = TRUE;
            thumb.rcDestination = RECT{ 0, 0, windowSize.cx, windowSize.cy };

            HTHUMBNAIL hThumb = nullptr;
            com_ptr<IDCompositionVisual2> rawVisual;

            HRESULT hr = g_DwmpCreateSharedThumbnailVisual(ghostHwnd, targetHwnd, 0, &thumb, g_dcompDevice.get(), (void**)rawVisual.put(), &hThumb);

            if (SUCCEEDED(hr) && rawVisual) {

                // Create the WinRT Bridge and Visual Tree
                auto compTarget = CreateDesktopWindowTarget(g_compositor, ghostHwnd);
                auto rootVisual = g_compositor.CreateContainerVisual();
                rootVisual.Opacity(0.0f);
                compTarget.Root(rootVisual);

                Visual activeVisual = DCompVisualToVisual(g_compositor, rawVisual.get(), windowSize);
                rootVisual.Children().InsertAtTop(activeVisual);

                // Stash everything into state to keep it alive
                DwmCaptureState* state = new DwmCaptureState();
                state->targetHwnd = targetHwnd;
                state->ghostHwnd = ghostHwnd;
                state->hThumb = hThumb;
                state->compTarget = compTarget;
                state->rootVisual = rootVisual;
                state->rawVisual = rawVisual;
                state->compVisual = activeVisual;
                state->currentSize = windowSize;

                g_activeDwmCaptures[targetHwnd] = state;
                g_dcompDevice->Commit();

                EngineLog("[DWM] Preloaded LIVE Capture for HWND: %p (Size: %dx%d)\n", targetHwnd, windowSize.cx, windowSize.cy);
            }
            else {
                DestroyWindow(ghostHwnd);
            }
        }
        catch (...) {}
        });

    return true;
}

extern "C" __declspec(dllexport) bool IsDwmCaptureReady(HWND targetHwnd) {
    if (g_isShuttingDown) return false;
    std::lock_guard<std::mutex> lock(g_captureMapMutex);
    return g_activeDwmCaptures.find(targetHwnd) != g_activeDwmCaptures.end();
}

extern "C" __declspec(dllexport) void StopDwmCaptureTest(HWND targetHwnd) {
    if (g_isShuttingDown || !g_queueController) return;

    auto queue = g_queueController.DispatcherQueue();
    queue.TryEnqueue([=]() {
        std::lock_guard<std::mutex> lock(g_captureMapMutex);
        auto it = g_activeDwmCaptures.find(targetHwnd);
        if (it != g_activeDwmCaptures.end()) {
            DwmCaptureState* state = it->second;
            if (state) {
                state->isClosing = true;
                try {
                    // Safely tear down the composition tree
                    if (state->compTarget) { state->compTarget.Root(nullptr); state->compTarget = nullptr; }
                    state->rootVisual = nullptr;
                    state->compVisual = nullptr;
                    state->rawVisual = nullptr;
                    state->compTarget = nullptr;

                    if (state->hThumb) { DwmUnregisterThumbnail(state->hThumb); }
                    if (state->ghostHwnd && IsWindow(state->ghostHwnd)) { DestroyWindow(state->ghostHwnd); }
                }
                catch (...) {}
                delete state;
            }
            g_activeDwmCaptures.erase(it);
            EngineLog("[DWM] Capture STOPPED and cleaned up for HWND: %p\n", targetHwnd);
        }
        });
}

extern "C" __declspec(dllexport) void TriggerDwmWindowMorph(
    HWND targetHwnd, int visX, int visY, float visW, float visH, int logicalX, int logicalY,
    int durationMs, float startSizeW, float startSizeH, float cornerRadius,
    float anchorX, float anchorY, float bezX1, float bezY1, float bezX2, float bezY2)
{
    if (!g_isCompositorReady || !g_dcompDevice) return;

    auto queue = g_queueController.DispatcherQueue();
    queue.TryEnqueue([=]() {
        try {
            DwmCaptureState* state = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_captureMapMutex);
                auto it = g_activeDwmCaptures.find(targetHwnd);
                if (it != g_activeDwmCaptures.end()) {
                    state = it->second;
                }
            }

            if (!state || !state->compVisual) {
                SetPropW(targetHwnd, L"TWM_EngineMove", (HANDLE)1);
                SetWindowPos(targetHwnd, nullptr, logicalX, logicalY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                RemovePropW(targetHwnd, L"TWM_EngineMove");
                SetLayeredWindowAttributes(targetHwnd, 0, 255, LWA_ALPHA);
                return;
            }

            // 1. Move the ghost window to the screen
            SetWindowPos(state->ghostHwnd, nullptr, visX, visY, (int)visW, (int)visH, SWP_NOZORDER | SWP_NOACTIVATE);

            // 2. Scale the WinRT visual to fit the new boundaries smoothly
            state->compVisual.Size({ (float)state->currentSize.cx, (float)state->currentSize.cy });
            state->compVisual.Scale({ visW / (float)state->currentSize.cx, visH / (float)state->currentSize.cy, 1.0f });

            state->rootVisual.Size({ visW, visH });

            // 3. Setup the Morph Geometry Clip
            float minDimension = std::min<float>(startSizeW, startSizeH);
            float startRadius = (minDimension / 2.0f) * cornerRadius;

            auto geometry = g_compositor.CreateRoundedRectangleGeometry();
            geometry.Size({ startSizeW, startSizeH });
            geometry.CornerRadius({ startRadius, startRadius });

            float startOffsetX = (visW - startSizeW) * anchorX;
            float startOffsetY = (visH - startSizeH) * anchorY;
            geometry.Offset({ startOffsetX, startOffsetY });

            auto clip = g_compositor.CreateGeometricClip(geometry);
            state->rootVisual.Clip(clip);

            state->rootVisual.Opacity(1.0f);

            ShowWindow(state->ghostHwnd, SW_SHOWNA);

            // 4. Fire the Animations
            auto batch = g_compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
            auto customBezier = g_compositor.CreateCubicBezierEasingFunction({ bezX1, bezY1 }, { bezX2, bezY2 });

            auto sizeAnim = g_compositor.CreateVector2KeyFrameAnimation();
            sizeAnim.InsertKeyFrame(1.0f, { visW, visH }, customBezier);
            sizeAnim.Duration(std::chrono::milliseconds(durationMs));

            auto offsetAnim = g_compositor.CreateVector2KeyFrameAnimation();
            offsetAnim.InsertKeyFrame(1.0f, { 0.0f, 0.0f }, customBezier);
            offsetAnim.Duration(std::chrono::milliseconds(durationMs));

            float endMinDimension = std::min<float>(visW, visH);
            float endRadius = std::min<float>(8.0f, endMinDimension / 2.0f);

            auto radiusAnim = g_compositor.CreateVector2KeyFrameAnimation();
            radiusAnim.InsertKeyFrame(0.0f, { startRadius, startRadius });
            radiusAnim.InsertKeyFrame(1.0f, { endRadius, endRadius }, customBezier);
            radiusAnim.Duration(std::chrono::milliseconds(durationMs));

            geometry.StartAnimation(L"Size", sizeAnim);
            geometry.StartAnimation(L"Offset", offsetAnim);
            geometry.StartAnimation(L"CornerRadius", radiusAnim);

            batch.End();

            HWND safeGhostHwnd = state->ghostHwnd;

            batch.Completed([safeGhostHwnd, targetHwnd, logicalX, logicalY](auto&&...) {
                try {
                    // Warp the real window to its final position
                    SetPropW(targetHwnd, L"TWM_EngineMove", (HANDLE)1);
                    SetWindowPos(targetHwnd, nullptr, logicalX, logicalY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    RemovePropW(targetHwnd, L"TWM_EngineMove");
                    SetLayeredWindowAttributes(targetHwnd, 0, 255, LWA_ALPHA);

                    // Instantly hide the ghost to avoid overlap flashing
                    if (safeGhostHwnd && IsWindow(safeGhostHwnd)) {
                        ShowWindow(safeGhostHwnd, SW_HIDE);
                    }
                }
                catch (...) {}
                });
        }
        catch (...) {}
        });
}

extern "C" __declspec(dllexport) void ShutdownEngine() {
    EngineLog("[SHUTDOWN] Halting all WGC and DWM operations...\n");
    g_isShuttingDown = true;
    g_isCompositorReady = false;

    if (g_queueController) {
        HANDLE hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
        auto queue = g_queueController.DispatcherQueue();

        queue.TryEnqueue([hEvent]() {
            std::lock_guard<std::mutex> lock(g_captureMapMutex);

            for (auto& pair : g_activeWgcCaptures) {
                if (pair.second) {
                    pair.second->isClosing = true;
                    try {
                        if (pair.second->session) { pair.second->session.Close(); }
                        if (pair.second->framePool) { pair.second->framePool.Close(); }
                    }
                    catch (...) {}
                    delete pair.second;
                }
            }
            g_activeWgcCaptures.clear();

            for (auto& pair : g_activeDwmCaptures) {
                if (pair.second) {
                    pair.second->isClosing = true;
                    try {
                        if (pair.second->hThumb) { DwmUnregisterThumbnail(pair.second->hThumb); }

                        if (pair.second->ghostHwnd && IsWindow(pair.second->ghostHwnd)) {
                            DestroyWindow(pair.second->ghostHwnd);
                        }
                    }
                    catch (...) {}
                    delete pair.second;
                }
            }
            g_activeDwmCaptures.clear();

            if (g_dwmOwnerHwnd) { DestroyWindow(g_dwmOwnerHwnd); g_dwmOwnerHwnd = nullptr; }

            g_compGraphics = nullptr;
            g_winrtDevice = nullptr;
            g_d3dDevice = nullptr;
            g_dcompDevice = nullptr;
            g_compositor = nullptr;

            if (hEvent) SetEvent(hEvent);
            });

        if (hEvent) {
            WaitForSingleObject(hEvent, 3000);
            CloseHandle(hEvent);
        }
    }

    g_queueController = nullptr;

    if (g_compositorThreadId != 0) {
        PostThreadMessageA(g_compositorThreadId, WM_QUIT, 0, 0);
    }
    if (g_compThread.joinable()) { g_compThread.join(); }
    EngineLog("[SHUTDOWN] Complete.\n");
}
