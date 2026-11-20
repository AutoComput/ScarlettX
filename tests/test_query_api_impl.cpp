#include "scarlettx/gdk/xgameruntime_abi.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "[test_query_api_impl] FAILED: " #cond " at line "      \
                      << __LINE__ << "\n";                                       \
            return 1;                                                            \
        }                                                                        \
    } while (0)

namespace {

struct TestGuid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

constexpr TestGuid kGuid073B7DCB = {
    0x073B7DCBu, 0x1FCFu, 0x4030u, {0x94, 0xBE, 0xE3, 0xC9, 0xEB, 0x62, 0x34, 0x28}};

constexpr TestGuid kGuidIUnknown = {
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

constexpr TestGuid kGuid37E56907 = {
    0x37E56907u, 0x2F10u, 0x41E8u, {0xB7, 0x2F, 0x36, 0xED, 0xB1, 0x85, 0x33, 0x1A}};

constexpr TestGuid kGuidUnsupported973A344E = {
    0x973A344Eu, 0x24BFu, 0x4D0Fu, {0x84, 0x57, 0x56, 0xC5, 0x34, 0x89, 0x2B, 0x29}};

constexpr XGameResult kENoInterface = static_cast<XGameResult>(0x80004002u);
constexpr XGameResult kENotImpl = static_cast<XGameResult>(0x80004001u);

template <typename Fn>
Fn LoadVtableSlot(void* obj, size_t byte_offset) {
    auto* vtbl_base = *reinterpret_cast<const uint8_t* const*>(obj);
    void* fn_ptr = nullptr;
    std::memcpy(&fn_ptr, vtbl_base + byte_offset, sizeof(fn_ptr));
    return reinterpret_cast<Fn>(fn_ptr);
}

using FnQueryInterface = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*, const void*, void**);
using FnAddRef = uint32_t(SCARLETTX_GDK_CALLBACK*)(void*);
using FnRelease = uint32_t(SCARLETTX_GDK_CALLBACK*)(void*);
using FnSlot06 = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*, XAsyncBlock*, XAsyncWork*);
using FnSlot12 = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*, uint32_t, uint32_t, XTaskQueueHandle*);
using FnSlot16 = uint8_t(SCARLETTX_GDK_CALLBACK*)(void*, XTaskQueueHandle, uint32_t, uint32_t);
using FnSlot17 = void(SCARLETTX_GDK_CALLBACK*)(void*, XTaskQueueHandle);
using FnSlot29 = void(SCARLETTX_GDK_CALLBACK*)(void*);
using FnUnimpl = XGameResult(SCARLETTX_GDK_CALLBACK*)(void*);

std::atomic<int> g_work_calls{0};
std::atomic<int> g_completion_calls{0};

XGameResult SCARLETTX_GDK_CALLBACK SampleAsyncWork(XAsyncBlock* async) {
    g_work_calls.fetch_add(1, std::memory_order_relaxed);
    auto* ctx_val = static_cast<int*>(async->context);
    if (ctx_val) {
        *ctx_val = 42;
    }
    return XGR_S_OK;
}

void SCARLETTX_GDK_CALLBACK SampleAsyncCompletion(XAsyncBlock* /*async*/) {
    g_completion_calls.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

int main() {
    CHECK(InitializeApiImplEx(19041, 10002, 0) == XGR_S_OK);

    // 1. Null parameter validation & out-pointer initialization.
    void* sentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEFull));
    void* out = sentinel;
    CHECK(QueryApiImpl(nullptr, &kGuid073B7DCB, &out) == XGR_E_POINTER);
    CHECK(QueryApiImpl(&kGuid073B7DCB, nullptr, &out) == XGR_E_POINTER);
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuid073B7DCB, nullptr) == XGR_E_POINTER);

    // 2. Unsupported GUIDs must fail with E_NOINTERFACE and null out *out_impl.
    out = sentinel;
    CHECK(QueryApiImpl(&kGuidUnsupported973A344E, &kGuidUnsupported973A344E, &out) == kENoInterface);
    CHECK(out == nullptr);

    out = sentinel;
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuidUnsupported973A344E, &out) == kENoInterface);
    CHECK(out == nullptr);

    // 2b. Second observed runtime GUID {37E56907-...} returns a ScarlettX-owned instance.
    void* impl_37e5 = nullptr;
    CHECK(QueryApiImpl(&kGuid37E56907, &kGuid37E56907, &impl_37e5) == XGR_S_OK);
    CHECK(impl_37e5 != nullptr);
    auto release_37e5 = LoadVtableSlot<FnRelease>(impl_37e5, 0x10);
    CHECK(release_37e5(impl_37e5) == 0u);

    // 3. Matching API GUID with identical IID and with IID_IUnknown.
    void* impl = nullptr;
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuid073B7DCB, &impl) == XGR_S_OK);
    CHECK(impl != nullptr);

    void* impl_unk = nullptr;
    CHECK(QueryApiImpl(&kGuid073B7DCB, &kGuidIUnknown, &impl_unk) == XGR_S_OK);
    CHECK(impl_unk != nullptr);
    CHECK(impl_unk != impl);  // ScarlettX-owned distinct guest instances.

    auto qi = LoadVtableSlot<FnQueryInterface>(impl, 0x00);
    auto add_ref = LoadVtableSlot<FnAddRef>(impl, 0x08);
    auto release = LoadVtableSlot<FnRelease>(impl, 0x10);
    auto unimpl = LoadVtableSlot<FnUnimpl>(impl, 0x18);
    auto slot_0x30 = LoadVtableSlot<FnSlot06>(impl, 0x30);
    auto slot_0x60 = LoadVtableSlot<FnSlot12>(impl, 0x60);
    auto slot_0x80 = LoadVtableSlot<FnSlot16>(impl, 0x80);
    auto slot_0x88 = LoadVtableSlot<FnSlot17>(impl, 0x88);
    auto slot_0xe8 = LoadVtableSlot<FnSlot29>(impl, 0xE8);

    CHECK(qi && add_ref && release && unimpl && slot_0x30 && slot_0x60 &&
          slot_0x80 && slot_0x88 && slot_0xe8);

    // 4. COM IUnknown lifetime checks at +0x00 / +0x08 / +0x10.
    void* qi_out = sentinel;
    CHECK(qi(impl, &kGuid073B7DCB, nullptr) == XGR_E_POINTER);
    CHECK(qi(impl, nullptr, &qi_out) == XGR_E_INVALIDARG);
    CHECK(qi_out == nullptr);

    qi_out = sentinel;
    CHECK(qi(impl, &kGuidUnsupported973A344E, &qi_out) == kENoInterface);
    CHECK(qi_out == nullptr);

    CHECK(qi(impl, &kGuidIUnknown, &qi_out) == XGR_S_OK);
    CHECK(qi_out == impl);
    CHECK(add_ref(impl) == 3u);
    CHECK(release(impl) == 2u);
    CHECK(release(qi_out) == 1u);

    // Release second instance cleanly to zero.
    auto release_unk = LoadVtableSlot<FnRelease>(impl_unk, 0x10);
    CHECK(release_unk(impl_unk) == 0u);

    // 5. Unimplemented slot returns E_NOTIMPL.
    CHECK(unimpl(impl) == kENotImpl);

    // 6. Observed slots: +0xE8, +0x60, +0x30, +0x80 (bool/u8), +0x88.
    slot_0xe8(impl);

    XTaskQueueHandle queue = nullptr;
    CHECK(slot_0x60(impl,
                    static_cast<uint32_t>(XTaskQueueDispatchMode_Manual),
                    static_cast<uint32_t>(XTaskQueueDispatchMode_Manual),
                    &queue) == XGR_S_OK);
    CHECK(queue != nullptr);

    // Empty manual queue returns 0 (false) in low byte.
    CHECK(slot_0x80(impl, queue, static_cast<uint32_t>(XTaskQueuePort_Work), 0) == 0u);

    int ctx_value = 0;
    XAsyncBlock async{};
    async.queue = queue;
    async.context = &ctx_value;
    async.callback = SampleAsyncCompletion;

    g_work_calls.store(0);
    g_completion_calls.store(0);
    CHECK(slot_0x30(impl, &async, SampleAsyncWork) == XGR_S_OK);

    // Dispatch work port: returns 1 (true) in low byte and runs callback.
    CHECK(slot_0x80(impl, queue, static_cast<uint32_t>(XTaskQueuePort_Work), 0) == 1u);
    CHECK(g_work_calls.load() == 1);
    CHECK(ctx_value == 42);

    // Dispatch completion port: returns 1 (true) in low byte and runs completion.
    CHECK(slot_0x80(impl, queue, static_cast<uint32_t>(XTaskQueuePort_Completion), 0) == 1u);
    CHECK(g_completion_calls.load() == 1);
    CHECK(XAsyncGetStatus(&async, false) == XGR_S_OK);

    // Close queue via +0x88 and release COM wrapper via +0x10.
    slot_0x88(impl, queue);
    CHECK(release(impl) == 0u);

    UninitializeApiImpl();
    std::cout << "[test_query_api_impl] PASSED\n";
    return 0;
}
