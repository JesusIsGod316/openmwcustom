#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <GL/gl.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace P9Dlss
{
    using Microsoft::WRL::ComPtr;
    using Clock = std::chrono::steady_clock;
    inline void require(bool value, const char* message)
    { if (!value) throw std::runtime_error(message); }
    inline void dx(HRESULT result, const char* call)
    {
        if (FAILED(result)) throw std::runtime_error(std::string(call) + ": HRESULT="
            + std::to_string(static_cast<unsigned long>(result)));
    }
    template<class F> F function(const char* name)
    {
        auto value = wglGetProcAddress(name);
        const auto bits = reinterpret_cast<std::uintptr_t>(value);
        require(value && bits > 3 && bits != ~std::uintptr_t(0), name);
        return reinterpret_cast<F>(value);
    }
    inline void glCheck(const char* call)
    {
        const auto error = glGetError();
        if (error) throw std::runtime_error(std::string(call) + ": GL=" + std::to_string(error));
    }
    inline double milliseconds(Clock::time_point start)
    { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

    struct HiddenContext
    {
        HWND window = nullptr;
        HDC dc = nullptr;
        HGLRC context = nullptr;
        HiddenContext()
        {
            WNDCLASSW cls{}; cls.style = CS_OWNDC; cls.lpfnWndProc = DefWindowProcW;
            cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"OptimizedMW-NGX-PrivateFixture";
            require(RegisterClassW(&cls) != 0, "RegisterClass private fixture");
            // No WS_VISIBLE or ShowWindow. The runner also assigns an unswitched desktop.
            window = CreateWindowW(cls.lpszClassName, L"OptimizedMW standalone DLSS adapter",
                WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, cls.hInstance, nullptr);
            require(window != nullptr && !IsWindowVisible(window), "Create hidden context window");
            dc = GetDC(window); require(dc != nullptr, "GetDC hidden context");
            PIXELFORMATDESCRIPTOR descriptor{}; descriptor.nSize = sizeof(descriptor);
            descriptor.nVersion = 1; descriptor.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW;
            descriptor.iPixelType = PFD_TYPE_RGBA; descriptor.cColorBits = 32;
            const int format = ChoosePixelFormat(dc, &descriptor);
            require(format != 0 && SetPixelFormat(dc, format, &descriptor), "Set GL pixel format");
            context = wglCreateContext(dc);
            require(context && wglMakeCurrent(dc, context), "Create current GL context");
        }
        void close()
        {
            require(wglMakeCurrent(nullptr, nullptr), "Release current GL context");
            require(wglDeleteContext(context), "Delete GL context"); context = nullptr;
            ReleaseDC(window, dc); dc = nullptr;
            require(DestroyWindow(window), "Destroy hidden fixture window"); window = nullptr;
        }
    };

    struct Bridge
    {
        HANDLE (WINAPI* openDevice)(void*) = function<decltype(openDevice)>("wglDXOpenDeviceNV");
        BOOL (WINAPI* closeDevice)(HANDLE) = function<decltype(closeDevice)>("wglDXCloseDeviceNV");
        HANDLE (WINAPI* registerObject)(HANDLE, void*, GLuint, GLenum, GLenum)
            = function<decltype(registerObject)>("wglDXRegisterObjectNV");
        BOOL (WINAPI* unregisterObject)(HANDLE, HANDLE) = function<decltype(unregisterObject)>("wglDXUnregisterObjectNV");
        BOOL (WINAPI* lockObjects)(HANDLE, GLint, HANDLE*) = function<decltype(lockObjects)>("wglDXLockObjectsNV");
        BOOL (WINAPI* unlockObjects)(HANDLE, GLint, HANDLE*) = function<decltype(unlockObjects)>("wglDXUnlockObjectsNV");
        HANDLE device = nullptr;
        void lock(std::vector<HANDLE>& values)
        { require(lockObjects(device, static_cast<GLint>(values.size()), values.data()), "GL/D3D lock"); }
        void unlock(std::vector<HANDLE>& values)
        { require(unlockObjects(device, static_cast<GLint>(values.size()), values.data()), "GL/D3D unlock"); }
    };
    struct Texture
    {
        ComPtr<ID3D11Texture2D> dx;
        GLuint gl = 0;
        HANDLE registration = nullptr;
        unsigned width = 0, height = 0;
    };

    struct Device
    {
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> commands;
        DXGI_ADAPTER_DESC description{};
        LUID glLuid{};
        UINT nodeMask = 0;
        Device()
        {
            const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
            require(renderer && std::strstr(renderer, "NVIDIA"), "Fixture requires the intended NVIDIA GL device");
            auto getId = function<void (APIENTRY*)(GLenum, GLubyte*)>("glGetUnsignedBytevEXT");
            getId(0x9599, reinterpret_cast<GLubyte*>(&glLuid)); // GL_DEVICE_LUID_EXT
            GLint mask = 0, count = 0;
            glGetIntegerv(0x9596, &count); // GL_NUM_DEVICE_UUIDS_EXT
            glGetIntegerv(0x959A, &mask); nodeMask = static_cast<UINT>(mask);
            glCheck("GL physical device identity");
            require(count == 1 && (glLuid.LowPart || glLuid.HighPart) && nodeMask && !(nodeMask & (nodeMask - 1)),
                "Nonzero GL device LUID/single device node");
            ComPtr<IDXGIFactory1> factory; dx(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
            for (UINT i = 0;; ++i)
            {
                ComPtr<IDXGIAdapter1> current;
                if (factory->EnumAdapters1(i, &current) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC desc{}; dx(current->GetDesc(&desc), "Get adapter identity");
                if (std::memcmp(&desc.AdapterLuid, &glLuid, sizeof(LUID)) == 0)
                { adapter = current; description = desc; break; }
            }
            require(adapter && description.VendorId == 0x10de, "Exact same NVIDIA adapter LUID");
            D3D_FEATURE_LEVEL level{};
            const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
            // No SINGLETHREADED flag: NV_DX_interop2 requires a multithread-capable device.
            dx(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                D3D11_SDK_VERSION, &device, &level, &commands), "D3D11CreateDevice");
            require(level >= D3D_FEATURE_LEVEL_11_0, "D3D11 feature level");
        }
        Texture make(Bridge& bridge, unsigned width, unsigned height, DXGI_FORMAT format, bool output)
        {
            require(width && height && width <= 1920 && height <= 1080, "Bounded 1080p fixture texture extent");
            UINT support = 0; dx(device->CheckFormatSupport(format, &support), "Check exact format support");
            const UINT required = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
                | (output ? D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW : 0);
            require((support & required) == required, "Exact texture usage support");
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
                | (output ? D3D11_BIND_UNORDERED_ACCESS : 0);
            Texture result; result.width = width; result.height = height;
            dx(device->CreateTexture2D(&desc, nullptr, &result.dx), "Create bounded shared texture");
            glGenTextures(1, &result.gl); glCheck("Create GL texture name");
            result.registration = bridge.registerObject(bridge.device, result.dx.Get(), result.gl,
                GL_TEXTURE_2D, 0x0001); // WGL_ACCESS_READ_WRITE_NV
            require(result.registration != nullptr, "Register exact D3D11 texture for OpenGL");
            return result;
        }
        void completion(const char* stage = "submitted fixture work")
        {
            ComPtr<ID3D11Query> event;
            D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
            dx(device->CreateQuery(&desc, &event), "Create retirement event");
            commands->End(event.Get()); commands->Flush();
            const auto deadline = Clock::now() + std::chrono::seconds(3);
            for (;;)
            {
                BOOL complete = FALSE;
                const HRESULT result = commands->GetData(event.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
                dx(result, "D3D11 retirement observation");
                if (result == S_OK && complete) return;
                if (Clock::now() >= deadline)
                    throw std::runtime_error(std::string("D3D11 retirement deadline at ") + stage
                        + "; device_removed_reason=" + std::to_string(
                            static_cast<unsigned long>(device->GetDeviceRemovedReason())));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    };
    inline void retire(Bridge& bridge, Texture& texture)
    {
        require(bridge.unregisterObject(bridge.device, texture.registration), "Retire shared GL registration");
        texture.registration = nullptr;
        glDeleteTextures(1, &texture.gl); glCheck("Retire GL texture name"); texture.gl = 0;
        texture.dx.Reset();
    }
}
