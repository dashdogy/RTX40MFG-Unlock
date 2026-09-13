#pragma once
#include <Windows.h>
#include <dxgi1_6.h>
struct FixturePresent {
    void* chain=nullptr;
    const DXGI_PRESENT_PARAMETERS* metadata=nullptr;
    const RECT* dirty=nullptr;
    const RECT* scroll=nullptr;
    const POINT* offset=nullptr;
    UINT sync=0,flags=0,dirtyCount=0,kind=0;
    HRESULT result=E_FAIL;
};

inline constexpr GUID kFixtureNativeIdentity={0x95f71528,0x7777,0x4d8a,{0xa3,0x15,0x60,0xca,0x09,0x12,0x10,0x03}};
inline void* FixtureNativeIdentity(IDXGISwapChain* chain){
    void* native=nullptr;UINT size=sizeof(native);
    return SUCCEEDED(chain->GetPrivateData(kFixtureNativeIdentity,&size,&native))?native:nullptr;
}
