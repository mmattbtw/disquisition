#include "desktop/windows_screen_capture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <thread>

using namespace ABI::Windows::Graphics::Capture;
using ABI::Windows::Foundation::IClosable;
using ABI::Windows::Foundation::TimeSpan;
using ABI::Windows::Graphics::SizeInt32;
using ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
using ABI::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface;

// MinUpdateInterval, Windows 11 24H2 and later. MinGW's headers predate it.
// Implemented by Windows, so this must not be in an anonymous namespace: GCC
// would see no implementation and compile calls on it as unreachable.
struct IGraphicsCaptureSession5 : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(TimeSpan* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(TimeSpan value) = 0;
};

namespace {

constexpr IID kSession5 = {0x67c0ea62, 0x1f85, 0x5061, {0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb}};

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { reset(); }
    void reset() {
        if (p) p->Release();
        p = nullptr;
    }
    T** put() {
        reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p; }
};

void close(IUnknown* object) {
    Com<IClosable> closable;
    if (object && SUCCEEDED(object->QueryInterface(__uuidof(IClosable),
                                                   reinterpret_cast<void**>(closable.put()))))
        closable->Close();
}

template <typename T>
HRESULT activationFactory(const wchar_t* name, T** factory) {
    HSTRING string = nullptr;
    HRESULT result = WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &string);
    if (SUCCEEDED(result)) {
        result = RoGetActivationFactory(string, __uuidof(T), reinterpret_cast<void**>(factory));
        WindowsDeleteString(string);
    }
    return result;
}

} // namespace

struct WindowsScreenCapture::State {
    std::function<void(QImage)> frame;
    std::function<void(QString)> error;
    int fps = 30;
    std::thread thread;
    std::atomic_bool stopping {false};
    std::atomic_bool active {false};

    bool start(HMONITOR monitor, HWND window) {
        stop();
        stopping = false;
        std::promise<bool> started;
        auto ready = started.get_future();
        thread = std::thread([this, monitor, window, started = std::move(started)]() mutable {
            run(monitor, window, started);
        });
        if (ready.get()) return true;
        thread.join();
        return false;
    }

    void stop() {
        stopping = true;
        if (thread.joinable()) thread.join();
        active = false;
    }

    void run(HMONITOR monitor, HWND window, std::promise<bool>& started) {
        const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
        bool reported = false;
        const auto fail = [&] {
            if (!reported) started.set_value(false);
            reported = true;
        };
        {
            Com<ID3D11Device> d3d;
            Com<ID3D11DeviceContext> context;
            Com<IDXGIDevice> dxgi;
            Com<IInspectable> inspectable;
            Com<IDirect3DDevice> device;
            Com<IGraphicsCaptureItemInterop> interop;
            Com<IGraphicsCaptureItem> item;
            Com<IDirect3D11CaptureFramePoolStatics2> poolStatics;
            Com<IDirect3D11CaptureFramePool> pool;
            Com<IGraphicsCaptureSession> session;
            SizeInt32 size {};
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                         D3D11_SDK_VERSION, d3d.put(), nullptr, context.put())) ||
                FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice),
                                           reinterpret_cast<void**>(dxgi.put()))) ||
                FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgi.p, inspectable.put())) ||
                FAILED(inspectable->QueryInterface(__uuidof(IDirect3DDevice),
                                                   reinterpret_cast<void**>(device.put()))) ||
                FAILED(activationFactory(L"Windows.Graphics.Capture.GraphicsCaptureItem",
                                         interop.put())) ||
                FAILED(window ? interop->CreateForWindow(window, __uuidof(IGraphicsCaptureItem),
                                                         reinterpret_cast<void**>(item.put()))
                              : interop->CreateForMonitor(monitor, __uuidof(IGraphicsCaptureItem),
                                                          reinterpret_cast<void**>(item.put()))) ||
                FAILED(item->get_Size(&size)) ||
                FAILED(activationFactory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool",
                                         poolStatics.put())) ||
                FAILED(poolStatics->CreateFreeThreaded(device.p,
                                                       DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                                       2, size, pool.put())) ||
                FAILED(pool->CreateCaptureSession(item.p, session.put()))) {
                fail();
            }
            else {
                Com<IGraphicsCaptureSession5> session5;
                if (SUCCEEDED(session->QueryInterface(kSession5,
                                                      reinterpret_cast<void**>(session5.put()))))
                    session5->put_MinUpdateInterval(TimeSpan {10'000}); // 1 ms
                Com<IGraphicsCaptureSession3> session3;
                if (SUCCEEDED(session->QueryInterface(__uuidof(IGraphicsCaptureSession3),
                                                      reinterpret_cast<void**>(session3.put()))))
                    session3->put_IsBorderRequired(false);
                if (FAILED(session->StartCapture())) {
                    fail();
                }
                else {
                    active = true;
                    reported = true;
                    started.set_value(true);
                    capture(d3d.p, context.p, device.p, pool.p, size, window);
                }
            }
            close(session.p);
            close(pool.p);
        }
        active = false;
        fail();
        if (SUCCEEDED(apartment)) RoUninitialize();
    }

    void capture(ID3D11Device* d3d, ID3D11DeviceContext* context, IDirect3DDevice* device,
                 IDirect3D11CaptureFramePool* pool, SizeInt32 poolSize, HWND window) {
        const qint64 minimumGapTicks = 10'000'000LL / fps * 3 / 4;
        qint64 lastTicks = 0;
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);
        Com<ID3D11Texture2D> staging;
        D3D11_TEXTURE2D_DESC stagingDesc {};
        ULONGLONG nextWindowCheck = 0;
        while (!stopping) {
            if (window && GetTickCount64() >= nextWindowCheck) {
                nextWindowCheck = GetTickCount64() + 500;
                if (!IsWindow(window)) {
                    error(QStringLiteral("the window is no longer available"));
                    break;
                }
            }
            Com<IDirect3D11CaptureFrame> frame;
            if (FAILED(pool->TryGetNextFrame(frame.put())) || !frame) {
                if (timer) {
                    LARGE_INTEGER due {};
                    due.QuadPart = -10'000; // 1 ms
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, 5);
                }
                else {
                    Sleep(1);
                }
                continue;
            }
            SizeInt32 content {};
            TimeSpan time {};
            frame->get_ContentSize(&content);
            frame->get_SystemRelativeTime(&time);
            if (content.Width != poolSize.Width || content.Height != poolSize.Height) {
                poolSize = content;
                pool->Recreate(device, DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, poolSize);
            }
            if (lastTicks && time.Duration - lastTicks < minimumGapTicks) {
                close(frame.p);
                continue;
            }
            lastTicks = time.Duration;
            QImage image = copyFrame(d3d, context, frame.p, content, staging, stagingDesc);
            close(frame.p);
            if (!image.isNull()) this->frame(std::move(image));
        }
        if (timer) CloseHandle(timer);
    }

    static QImage copyFrame(ID3D11Device* d3d, ID3D11DeviceContext* context,
                            IDirect3D11CaptureFrame* frame, SizeInt32 content,
                            Com<ID3D11Texture2D>& staging, D3D11_TEXTURE2D_DESC& stagingDesc) {
        Com<IDirect3DSurface> surface;
        Com<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        Com<ID3D11Texture2D> texture;
        if (FAILED(frame->get_Surface(surface.put())) ||
            FAILED(surface->QueryInterface(
                __uuidof(Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess),
                reinterpret_cast<void**>(access.put()))) ||
            FAILED(access->GetInterface(__uuidof(ID3D11Texture2D),
                                        reinterpret_cast<void**>(texture.put())))) {
            return {};
        }
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        if (!staging || stagingDesc.Width != desc.Width || stagingDesc.Height != desc.Height) {
            stagingDesc = desc;
            stagingDesc.Usage = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            stagingDesc.BindFlags = 0;
            stagingDesc.MiscFlags = 0;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.SampleDesc = {1, 0};
            if (FAILED(d3d->CreateTexture2D(&stagingDesc, nullptr, staging.put()))) return {};
        }
        context->CopyResource(staging.p, texture.p);
        D3D11_MAPPED_SUBRESOURCE mapped {};
        if (FAILED(context->Map(staging.p, 0, D3D11_MAP_READ, 0, &mapped))) return {};
        // A window's texture can be larger than its content while it resizes.
        const int width = std::min<int>(content.Width, static_cast<int>(desc.Width));
        const int height = std::min<int>(content.Height, static_cast<int>(desc.Height));
        QImage image;
        if (width > 0 && height > 0) {
            image = QImage(width, height, QImage::Format_RGB32);
            const auto* source = static_cast<const uchar*>(mapped.pData);
            for (int y = 0; y < height; ++y)
                std::memcpy(image.scanLine(y), source + static_cast<size_t>(y) * mapped.RowPitch,
                            static_cast<size_t>(width) * 4);
        }
        context->Unmap(staging.p, 0);
        return image;
    }
};

WindowsScreenCapture::WindowsScreenCapture(std::function<void(QImage)> frame,
                                           std::function<void(QString)> error, int fps)
    : state_(std::make_unique<State>()) {
    state_->frame = std::move(frame);
    state_->error = std::move(error);
    state_->fps = std::max(1, fps);
}

WindowsScreenCapture::~WindowsScreenCapture() {
    stop();
}

bool WindowsScreenCapture::startMonitor(quintptr monitor) {
    return monitor && state_->start(reinterpret_cast<HMONITOR>(monitor), nullptr);
}

bool WindowsScreenCapture::startWindow(quintptr window) {
    return window && state_->start(nullptr, reinterpret_cast<HWND>(window));
}

void WindowsScreenCapture::stop() {
    state_->stop();
}

bool WindowsScreenCapture::running() const {
    return state_->active;
}
