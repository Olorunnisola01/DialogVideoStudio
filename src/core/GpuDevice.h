#pragma once

#include <string>

#include <onnxruntime_cxx_api.h>

namespace dvs {

struct GpuDeviceInfo {
    // DXGI adapter index suitable for OrtSessionOptionsAppendExecutionProvider_DML,
    // or -1 if no usable hardware adapter was found.
    int deviceId = -1;
    std::string name;
};

// Enumerates DXGI adapters and picks the one with the most dedicated video
// memory, skipping software/WARP adapters. On a system with both a discrete
// GPU and an integrated GPU the discrete one is preferred. Returns
// deviceId == -1 if DXGI enumeration fails or no hardware adapter is found.
GpuDeviceInfo selectBestDmlDevice();

// Attempts to append the DirectML execution provider (using the adapter from
// selectBestDmlDevice) to `opts` and disables memory pattern reuse as required
// by the DML EP. Returns true and sets *gpuName on success. Returns false
// (leaving `opts` unmodified) if no GPU is available, DML EP creation throws,
// or isGpuPreferred() is false.
bool tryEnableDml(Ort::SessionOptions& opts, std::string* gpuName);

// Runtime user preference for whether the diarizer should attempt DirectML GPU
// acceleration. Defaults to true unless DVS_FORCE_CPU is set in the
// environment at startup.
void setGpuPreferred(bool preferred);
bool isGpuPreferred();

} // namespace dvs
