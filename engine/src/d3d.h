#pragma once

// Direct3D 11 and DXGI, for the engine's own files. Nothing of this shows in the engine's headers.

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <qkvm/base/result.h>

namespace qkvm::engine {

using Microsoft::WRL::ComPtr;

// The error for a call into Direct3D or DXGI that failed: what was being done, and Windows' number.
base::Error graphicsError(const char* doing, HRESULT result);

// Whether a failed call says that Windows has taken the device away (DESIGN.md section 3.4).
inline bool meansDeviceLost(HRESULT result) noexcept
{
    return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET ||
           result == DXGI_ERROR_DEVICE_HUNG || result == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

// The code of the error a caller gets while the device is gone.
inline constexpr const char* deviceLostCode = "device_lost";

} // namespace qkvm::engine
