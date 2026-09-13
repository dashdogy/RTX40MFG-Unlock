#pragma once
#include "single_module_status.h"
#include <dxgi1_6.h>
namespace single_overlay::proxy {
using FactoryFn=HRESULT (WINAPI*)(REFIID,void**);
using Factory2Fn=HRESULT (WINAPI*)(UINT,REFIID,void**);
HRESULT FactoryCall(FactoryFn,REFIID,void**) noexcept;
HRESULT FactoryCall(Factory2Fn,UINT,REFIID,void**) noexcept;
void ReadStatus(MfgSingleModuleStatus&) noexcept;
}
