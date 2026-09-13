#pragma once
#include <dxgi1_6.h>
namespace single_overlay::dx12 {
struct Session;
Session* CreateSession(IDXGISwapChain*, IUnknown*) noexcept;
void DestroySession(Session*) noexcept;
bool BeginPresent(Session*, UINT flags, bool partial) noexcept;
void EndPresent(Session*, HRESULT, UINT flags) noexcept;
bool BeginResize(Session*) noexcept;
void EndResize(Session*, HRESULT) noexcept;
void ObserveColorSpace(Session*, DXGI_COLOR_SPACE_TYPE) noexcept;
void VerifyResizeQueues(Session*, UINT, IUnknown* const*) noexcept;
}
