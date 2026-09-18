#pragma once
#include <unknwn.h>
namespace single_overlay::adapter_parent {
// Observe the exact adapter supplied to a public D3D12CreateDevice call.
// The adapter pointer and all native interface tables remain unchanged.
void Observe(IUnknown*) noexcept;
}
