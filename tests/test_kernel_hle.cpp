#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "scarlettx/kernel/xmemory.h"
#include "scarlettx/kernel/xruntime.h"
#include "scarlettx/kernel/xstorage.h"
#include "scarlettx/loader/package_vfs.h"

int main() {
    // 1. Unified Memory Manager (64KB & 2MB pages)
    scarlettx::UnifiedMemoryManager mem;
    scarlettx::UnifiedMemoryBlock b1{};
    auto st = mem.Allocate(10000, scarlettx::MemoryPageSize::Page64KB, true, true, b1);
    assert(st.ok());
    assert(b1.size == 65536);
    assert((b1.gpu_va % 65536) == 0);
    assert(b1.host_ptr != nullptr);
    std::memset(b1.host_ptr, 0xAB, 256);

    scarlettx::UnifiedMemoryBlock found{};
    assert(mem.LookupByGpuVa(b1.gpu_va + 128, found));
    assert(found.gpu_va == b1.gpu_va);
    assert(mem.ActiveAllocationCount() == 1);

    st = mem.Free(b1.gpu_va);
    assert(st.ok());
    assert(mem.ActiveAllocationCount() == 0);
    assert(mem.TotalAllocatedBytes() == 0);

    // 2. XGameRuntime + XTaskQueue
    auto& rt = scarlettx::XGameRuntimeHle::Instance();
    assert(rt.Initialize(scarlettx::DeviceType::XboxScarlettAnaconda).ok());
    assert(rt.GetDeviceType() == scarlettx::DeviceType::XboxScarlettAnaconda);

    scarlettx::XUserProfile user;
    assert(rt.GetDefaultUser(user));
    assert(user.signed_in && user.xuid != 0);

    const auto qh = rt.CreateTaskQueue();
    auto* q = rt.GetTaskQueue(qh);
    assert(q != nullptr);
    int counter = 0;
    q->SubmitWork([&]() { counter += 10; });
    q->SubmitCompletion([&]() { counter += 5; });
    assert(q->DispatchWork() == 1);
    assert(q->DispatchCompletion() == 1);
    assert(counter == 15);
    rt.CloseTaskQueue(qh);
    rt.Shutdown();

    // 3. PackageVfs + XStorageQueueHle
    const auto tmp_dir = std::filesystem::path(SCARLETTX_BINARY_DIR) / "tmp_vfs_test";
    std::filesystem::create_directories(tmp_dir);
    const auto data_file = tmp_dir / "asset.bin";
    {
        std::ofstream ofs(data_file, std::ios::binary);
        ofs << "SCARLETTX_DIRECTSTORAGE_PAYLOAD";
    }

    scarlettx::PackageVfs vfs;
    assert(vfs.MountDirectory("G:", tmp_dir.string()).ok());
    assert(vfs.Exists("G:\\asset.bin"));

    scarlettx::XStorageQueueHle storage(vfs);
    std::vector<uint8_t> dst(13, 0);
    storage.EnqueueRead({"G:\\asset.bin", 10, 13, dst.data()});
    auto results = storage.SubmitAndWait();
    assert(results.size() == 1);
    assert(results[0].status.ok());
    assert(results[0].bytes_read == 13);
    assert(std::string(dst.begin(), dst.end()) == "DIRECTSTORAGE");

    std::cout << "[test_kernel_hle] PASSED\n";
    return 0;
}
