#define SCARLETTX_BUILD_XGAMERUNTIME
#include "scarlettx/gpu/d3d12_xs_abi.h"

#include "scarlettx/kernel/xruntime.h"

extern "C" {

int32_t XGameRuntimeInitialize() {
    auto st = scarlettx::XGameRuntimeHle::Instance().Initialize(
        scarlettx::DeviceType::XboxScarlettAnaconda);
    return st.ok() ? 0 : static_cast<int32_t>(st.code);
}

void XGameRuntimeUninitialize() {
    scarlettx::XGameRuntimeHle::Instance().Shutdown();
}

uint32_t XSystemGetDeviceType() {
    return static_cast<uint32_t>(scarlettx::XGameRuntimeHle::Instance().GetDeviceType());
}

int32_t XUserGetDefaultXuid(uint64_t* out_xuid) {
    if (!out_xuid) {
        return -1;
    }
    scarlettx::XUserProfile user;
    if (!scarlettx::XGameRuntimeHle::Instance().GetDefaultUser(user)) {
        return -2;
    }
    *out_xuid = user.xuid;
    return 0;
}

int32_t XTaskQueueCreate(uint64_t* out_queue_handle) {
    if (!out_queue_handle) {
        return -1;
    }
    *out_queue_handle = scarlettx::XGameRuntimeHle::Instance().CreateTaskQueue();
    return 0;
}

int32_t XTaskQueueCloseHandle(uint64_t queue_handle) {
    scarlettx::XGameRuntimeHle::Instance().CloseTaskQueue(queue_handle);
    return 0;
}

}  // extern "C"
