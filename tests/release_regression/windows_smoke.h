#pragma once
#include <Windows.h>
#include <unknwn.h>
#include <cstring>

// Calls are local capability/argument checks. They create no graphics device,
// swapchain, network connection, sound buffer, or controller vibration.
inline bool WindowsProxySmoke(HMODULE proxy, HMODULE original, const wchar_t* name)
{
    auto get = [](HMODULE module, const char* symbol) { return GetProcAddress(module, symbol); };
    if (_wcsicmp(name, L"d3d9.dll") == 0)
    {
        using Fn = void* (WINAPI*)(UINT);
        auto a = reinterpret_cast<Fn>(get(proxy, "Direct3DCreate9"));
        auto b = reinterpret_cast<Fn>(get(original, "Direct3DCreate9"));
        return a && b && !a(0) && !b(0);
    }
    if (_wcsicmp(name, L"d3d10.dll") == 0)
    {
        using Fn = HRESULT (WINAPI*)(void*, UINT, HMODULE, UINT, UINT, void**);
        auto a = reinterpret_cast<Fn>(get(proxy, "D3D10CreateDevice"));
        auto b = reinterpret_cast<Fn>(get(original, "D3D10CreateDevice"));
        void* x = nullptr; void* y = nullptr;
        if (!a || !b) return false;
        const HRESULT first = a(nullptr, 0, nullptr, 0, 0, &x);
        return FAILED(first) && first == b(nullptr, 0, nullptr, 0, 0, &y) && !x && !y;
    }
    if (_wcsicmp(name, L"d3d11.dll") == 0)
    {
        using Fn = HRESULT (WINAPI*)(void*, UINT, HMODULE, UINT, const UINT*, UINT,
            UINT, void**, UINT*, void**);
        auto a = reinterpret_cast<Fn>(get(proxy, "D3D11CreateDevice"));
        auto b = reinterpret_cast<Fn>(get(original, "D3D11CreateDevice"));
        void* x = nullptr; void* y = nullptr;
        if (!a || !b) return false;
        const HRESULT first = a(nullptr, 0, nullptr, 0, nullptr, 0, 0, &x, nullptr, nullptr);
        return FAILED(first) && first == b(nullptr, 0, nullptr, 0, nullptr, 0, 0, &y, nullptr, nullptr) && !x && !y;
    }
    if (_wcsicmp(name, L"d3d12.dll") == 0)
    {
        using Fn = HRESULT (WINAPI*)(REFCLSID, REFIID, void**);
        auto a = reinterpret_cast<Fn>(get(proxy, "D3D12GetInterface"));
        auto b = reinterpret_cast<Fn>(get(original, "D3D12GetInterface"));
        const GUID unknown{};
        void* x = nullptr; void* y = nullptr;
        if (!a || !b) return false;
        const HRESULT first = a(unknown, unknown, &x);
        return FAILED(first) && first == b(unknown, unknown, &y) && !x && !y;
    }
    if (_wcsicmp(name, L"dxgi.dll") == 0)
    {
        using Fn = HRESULT (WINAPI*)(REFIID, void**);
        auto a = reinterpret_cast<Fn>(get(proxy, "CreateDXGIFactory1"));
        auto b = reinterpret_cast<Fn>(get(original, "CreateDXGIFactory1"));
        const GUID iid = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
        IUnknown* x = nullptr; IUnknown* y = nullptr;
        if (!a || !b) return false;
        const HRESULT first = a(iid, reinterpret_cast<void**>(&x));
        const HRESULT second = b(iid, reinterpret_cast<void**>(&y));
        const bool passed = SUCCEEDED(first) && first == second && x && y;
        if (x) x->Release(); if (y) y->Release();
        return passed;
    }
    if (_wcsicmp(name, L"dsound.dll") == 0)
    {
        using Fn = HRESULT (WINAPI*)(LPCGUID, LPGUID);
        auto a = reinterpret_cast<Fn>(get(proxy, "GetDeviceID"));
        auto b = reinterpret_cast<Fn>(get(original, "GetDeviceID"));
        if (!a || !b) return false;
        const HRESULT first = a(nullptr, nullptr);
        return FAILED(first) && first == b(nullptr, nullptr);
    }
    if (_wcsicmp(name, L"wininet.dll") == 0)
    {
        using Fn = BOOL (WINAPI*)(LPDWORD, DWORD);
        auto a = reinterpret_cast<Fn>(get(proxy, "InternetGetConnectedState"));
        auto b = reinterpret_cast<Fn>(get(original, "InternetGetConnectedState"));
        DWORD x = 0; DWORD y = 0;
        return a && b && a(&x, 0) == b(&y, 0) && x == y;
    }
    if (_wcsicmp(name, L"winhttp.dll") == 0)
    {
        using Fn = BOOL (WINAPI*)(LPCWSTR, LPSYSTEMTIME);
        auto a = reinterpret_cast<Fn>(get(proxy, "WinHttpTimeToSystemTime"));
        auto b = reinterpret_cast<Fn>(get(original, "WinHttpTimeToSystemTime"));
        SYSTEMTIME x{}; SYSTEMTIME y{};
        constexpr auto time = L"Sun, 06 Nov 1994 08:49:37 GMT";
        return a && b && a(time, &x) && b(time, &y) && memcmp(&x, &y, sizeof(x)) == 0;
    }
    if (_wcsnicmp(name, L"xinput", 6) == 0)
    {
        using Fn = DWORD (WINAPI*)(DWORD, void*);
        auto a = reinterpret_cast<Fn>(get(proxy, "XInputGetState"));
        auto b = reinterpret_cast<Fn>(get(original, "XInputGetState"));
        unsigned char x[16]{}; unsigned char y[16]{};
        if (!a || !b) return false;
        const DWORD first = a(0xffffffff, x);
        return first != ERROR_SUCCESS && first == b(0xffffffff, y) && memcmp(x, y, sizeof(x)) == 0;
    }
    return true; // Existing harness covers version, dinput8 and winmm.
}
