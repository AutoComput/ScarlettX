#include "scarlettx/gdk/xgameruntime_abi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#endif

#include "scarlettx/kernel/xruntime.h"

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Global handle registry (for XSystemIsHandleValid)
// ---------------------------------------------------------------------------
std::mutex g_valid_handles_mutex;
std::unordered_set<void*> g_valid_handles;

void RegisterValidHandle(void* handle) {
    if (!handle) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_valid_handles_mutex);
    g_valid_handles.insert(handle);
}

void UnregisterValidHandle(void* handle) {
    if (!handle) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_valid_handles_mutex);
    g_valid_handles.erase(handle);
}

bool IsRegisteredHandle(void* handle) {
    if (!handle) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_valid_handles_mutex);
    return g_valid_handles.count(handle) > 0;
}

// ---------------------------------------------------------------------------
// Helper for copying C strings into GDK output buffers
// ---------------------------------------------------------------------------
XGameResult CopyStringToBuffer(const std::string& src,
                               size_t dst_size,
                               char* dst,
                               size_t* used) {
    const size_t required = src.size() + 1;
    if (used) {
        *used = required;
    }
    if (!dst || dst_size < required) {
        return XGR_E_NOT_SUFFICIENT_BUFFER;
    }
    std::memcpy(dst, src.c_str(), required);
    return XGR_S_OK;
}

// ---------------------------------------------------------------------------
// XTaskQueue implementation
// ---------------------------------------------------------------------------
struct TaskItem {
    XTaskQueueCallback* callback = nullptr;
    void* context = nullptr;
    std::chrono::steady_clock::time_point ready_at;
};

struct MonitorEntry {
    uint64_t token = 0;
    XTaskQueueMonitorCallback* callback = nullptr;
    void* context = nullptr;
};

struct TaskPortState {
    XTaskQueueDispatchMode mode = XTaskQueueDispatchMode_Manual;
    std::deque<TaskItem> items;
    std::thread worker;
    bool has_worker = false;
};

}  // namespace

struct XTaskQueueObject {
    std::atomic<int32_t> ref_count{1};
    std::mutex mutex;
    std::condition_variable cv;
    TaskPortState ports[2];
    bool terminating = false;
    bool terminated = false;
    std::vector<MonitorEntry> monitors;
    uint64_t next_monitor_token = 1;
    std::vector<std::pair<XTaskQueueTerminatedCallback*, void*>> terminated_callbacks;
};

namespace {

void NotifyMonitors(XTaskQueueObject* q, XTaskQueuePort port) {
    std::vector<MonitorEntry> snapshot;
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        snapshot = q->monitors;
    }
    for (const auto& m : snapshot) {
        if (m.callback) {
            m.callback(m.context, q, port);
        }
    }
}

bool PopReadyItemLocked(TaskPortState& port_state,
                        TaskItem& out_item,
                        std::chrono::steady_clock::time_point& out_next_ready) {
    const auto now = std::chrono::steady_clock::now();
    bool has_delayed = false;
    for (auto it = port_state.items.begin(); it != port_state.items.end(); ++it) {
        if (it->ready_at <= now) {
            out_item = *it;
            port_state.items.erase(it);
            return true;
        }
        if (!has_delayed || it->ready_at < out_next_ready) {
            out_next_ready = it->ready_at;
            has_delayed = true;
        }
    }
    return false;
}

void PortWorkerLoop(XTaskQueueObject* q, XTaskQueuePort port) {
    const size_t idx = static_cast<size_t>(port);
    for (;;) {
        TaskItem item;
        bool has_item = false;
        bool canceled = false;
        {
            std::unique_lock<std::mutex> lock(q->mutex);
            for (;;) {
                std::chrono::steady_clock::time_point next_ready;
                if (PopReadyItemLocked(q->ports[idx], item, next_ready)) {
                    has_item = true;
                    canceled = q->terminating;
                    break;
                }
                if (q->terminating) {
                    if (!q->ports[idx].items.empty()) {
                        item = q->ports[idx].items.front();
                        q->ports[idx].items.pop_front();
                        has_item = true;
                        canceled = true;
                        break;
                    }
                    return;
                }
                if (!q->ports[idx].items.empty()) {
                    q->cv.wait_until(lock, next_ready);
                } else {
                    q->cv.wait(lock);
                }
            }
        }
        if (has_item && item.callback) {
            item.callback(item.context, canceled);
        }
    }
}

void StartPortWorkerIfNeeded(XTaskQueueObject* q, XTaskQueuePort port) {
    const size_t idx = static_cast<size_t>(port);
    auto mode = q->ports[idx].mode;
    if (mode == XTaskQueueDispatchMode_ThreadPool ||
        mode == XTaskQueueDispatchMode_SerializedThreadPool) {
        q->ports[idx].has_worker = true;
        q->ports[idx].worker = std::thread(PortWorkerLoop, q, port);
    }
}

void TerminateQueueInternal(XTaskQueueObject* q, bool wait) {
    std::vector<TaskItem> canceled_manual_items;
    std::vector<std::pair<XTaskQueueTerminatedCallback*, void*>> term_cbs;
    {
        std::lock_guard<std::mutex> lock(q->mutex);
        q->terminating = true;
        for (int i = 0; i < 2; ++i) {
            if (!q->ports[i].has_worker) {
                while (!q->ports[i].items.empty()) {
                    canceled_manual_items.push_back(q->ports[i].items.front());
                    q->ports[i].items.pop_front();
                }
            }
        }
    }
    q->cv.notify_all();

    for (const auto& item : canceled_manual_items) {
        if (item.callback) {
            item.callback(item.context, true);
        }
    }

    if (wait) {
        for (int i = 0; i < 2; ++i) {
            if (q->ports[i].has_worker && q->ports[i].worker.joinable()) {
                if (q->ports[i].worker.get_id() != std::this_thread::get_id()) {
                    q->ports[i].worker.join();
                } else {
                    q->ports[i].worker.detach();
                }
                q->ports[i].has_worker = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(q->mutex);
            q->terminated = true;
            term_cbs.swap(q->terminated_callbacks);
        }
        for (const auto& cb : term_cbs) {
            if (cb.first) {
                cb.first(cb.second);
            }
        }
    }
}

std::mutex g_process_queue_mutex;
XTaskQueueHandle g_process_queue = nullptr;

XTaskQueueHandle EnsureDefaultProcessQueue() {
    std::lock_guard<std::mutex> lock(g_process_queue_mutex);
    if (!g_process_queue) {
        XTaskQueueHandle created = nullptr;
        if (XTaskQueueCreate(XTaskQueueDispatchMode_ThreadPool,
                             XTaskQueueDispatchMode_ThreadPool,
                             &created) == XGR_S_OK) {
            g_process_queue = created;
        }
    }
    return g_process_queue;
}

// ---------------------------------------------------------------------------
// XAsync internal state stored in XAsyncBlock::internal
// ---------------------------------------------------------------------------
struct XAsyncState {
    std::mutex mutex;
    std::condition_variable cv;
    XGameResult status = XGR_E_PENDING;
    bool completed = false;
    bool canceled = false;
    size_t result_size = 0;
    std::vector<uint8_t> result_payload;
    void* result_handle = nullptr;
    XAsyncBlock* block = nullptr;
    XTaskQueueHandle queue = nullptr;
    std::function<void(XAsyncState*)> work_fn;
};

// Map XAsyncBlock* -> shared_ptr<XAsyncState> for safe lifetime management.
std::mutex g_async_map_mutex;
std::unordered_map<XAsyncBlock*, std::shared_ptr<XAsyncState>> g_async_states;

std::shared_ptr<XAsyncState> GetAsyncState(XAsyncBlock* block) {
    if (!block) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_async_map_mutex);
    auto it = g_async_states.find(block);
    return it != g_async_states.end() ? it->second : nullptr;
}

void SCARLETTX_GDK_CALLBACK AsyncCompletionTrampoline(void* context, bool /*canceled*/) {
    auto* raw_state = static_cast<XAsyncState*>(context);
    if (!raw_state) {
        return;
    }
    XAsyncBlock* block = raw_state->block;
    XAsyncCompletionRoutine* cb = block ? block->callback : nullptr;
    if (cb) {
        cb(block);
    }
}

void SCARLETTX_GDK_CALLBACK AsyncWorkTrampoline(void* context, bool canceled) {
    auto* state = static_cast<XAsyncState*>(context);
    if (!state) {
        return;
    }
    if (canceled) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->completed) {
            state->canceled = true;
            state->status = XGR_E_ABORT;
            state->completed = true;
        }
        state->cv.notify_all();
    } else if (state->work_fn) {
        state->work_fn(state);
    }

    if (state->block && state->block->callback && state->queue) {
        XTaskQueueSubmitCallback(
            state->queue,
            XTaskQueuePort_Completion,
            state,
            AsyncCompletionTrampoline);
    }
}

std::shared_ptr<XAsyncState> BeginAsyncOperation(
    XAsyncBlock* async,
    std::function<void(XAsyncState*)> work_fn) {
    auto state = std::make_shared<XAsyncState>();
    state->block = async;
    state->work_fn = std::move(work_fn);

    XTaskQueueHandle q = async->queue;
    if (!q) {
        q = EnsureDefaultProcessQueue();
    }
    state->queue = q;

    // Store raw pointer marker in internal buffer for ABI inspection.
    std::memset(async->internal, 0, sizeof(async->internal));
    XAsyncState* raw = state.get();
    std::memcpy(async->internal, &raw, sizeof(raw));

    {
        std::lock_guard<std::mutex> lock(g_async_map_mutex);
        g_async_states[async] = state;
    }

    XTaskQueueSubmitCallback(q, XTaskQueuePort_Work, raw, AsyncWorkTrampoline);
    return state;
}

void CompleteAsyncState(XAsyncState* state, XGameResult hr, size_t result_size = 0) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->completed) {
        state->status = state->canceled ? XGR_E_ABORT : hr;
        state->result_size = result_size;
        state->completed = true;
    }
    state->cv.notify_all();
}

// ---------------------------------------------------------------------------
// XUser implementation
// ---------------------------------------------------------------------------
}  // namespace

struct XUser {
    std::atomic<int32_t> ref_count{1};
    uint64_t xuid = 0x00090000005CA001ull;
    uint64_t local_id = 1;
    std::string gamertag_classic = "ScarlettXUser";
    std::string gamertag_modern = "ScarlettX";
    std::string gamertag_suffix = "1001";
    std::string gamertag_unique = "ScarlettX#1001";
    XUserState state = XUserState_SignedIn;
    XUserAgeGroup age_group = XUserAgeGroup_Adult;
    bool is_guest = false;
};

// ---------------------------------------------------------------------------
// XGameSave implementation (real persistent local-disk containers & blobs)
// ---------------------------------------------------------------------------
struct XGameSaveProvider {
    std::atomic<int32_t> ref_count{1};
    std::string config_id;
    fs::path root_dir;
    uint64_t max_quota_bytes = 256ull * 1024ull * 1024ull;  // 256 MiB
    std::mutex mutex;
};

struct XGameSaveContainer {
    XGameSaveProvider* provider = nullptr;
    std::string name;
    fs::path dir_path;
};

struct XGameSaveUpdate {
    XGameSaveContainer* container = nullptr;
    std::string display_name;
    std::map<std::string, std::vector<uint8_t>> pending_writes;
    std::vector<std::string> pending_deletes;
};

namespace {

std::string SanitizeSaveName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "default" : out;
}

fs::path ResolveSaveRoot(const std::string& config_id) {
    const char* env_root = std::getenv("SCARLETTX_SAVE_ROOT");
    fs::path base = (env_root && *env_root)
                        ? fs::path(env_root)
                        : (fs::temp_directory_path() / "scarlettx_saves");
    return base / SanitizeSaveName(config_id);
}

uint64_t ComputeDirectorySize(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) {
        return 0;
    }
    uint64_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            total += entry.file_size(ec);
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// XMem hooks
// ---------------------------------------------------------------------------
std::mutex g_mem_hooks_mutex;
XMemAllocFunction* g_mem_alloc_hook = nullptr;
XMemFreeFunction* g_mem_free_hook = nullptr;

// ---------------------------------------------------------------------------
// XThread TLS flag
// ---------------------------------------------------------------------------
thread_local bool g_thread_time_sensitive = false;

}  // namespace

extern "C" {

// ===========================================================================
// XGameRuntimeInit.h
// ===========================================================================
XGameResult XGameRuntimeInitialize() {
    auto st = scarlettx::XGameRuntimeHle::Instance().Initialize(
        scarlettx::DeviceType::XboxScarlettAnaconda);
    if (!st.ok()) {
        return XGR_E_FAIL;
    }
    EnsureDefaultProcessQueue();
    return XGR_S_OK;
}

XGameResult XGameRuntimeInitializeWithOptions(const XGameRuntimeOptions* /*options*/) {
    return XGameRuntimeInitialize();
}

void XGameRuntimeUninitialize() {
    {
        std::lock_guard<std::mutex> lock(g_process_queue_mutex);
        if (g_process_queue) {
            XTaskQueueHandle q = g_process_queue;
            g_process_queue = nullptr;
            XTaskQueueTerminate(q, true, nullptr, nullptr);
            XTaskQueueCloseHandle(q);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_async_map_mutex);
        g_async_states.clear();
    }
    scarlettx::XGameRuntimeHle::Instance().Shutdown();
}

XGameResult XGameRuntimeIsFeatureAvailable(XGameRuntimeFeature feature, bool* isAvailable) {
    if (!isAvailable) {
        return XGR_E_POINTER;
    }
    switch (feature) {
        case XGameRuntimeFeature_XTaskQueue:
        case XGameRuntimeFeature_XAsync:
        case XGameRuntimeFeature_XUser:
        case XGameRuntimeFeature_XGameSave:
        case XGameRuntimeFeature_XSystem:
        case XGameRuntimeFeature_XMem:
        case XGameRuntimeFeature_XThread:
            *isAvailable = true;
            return XGR_S_OK;
        default:
            *isAvailable = false;
            return XGR_S_OK;
    }
}

// ===========================================================================
// XTaskQueue.h
// ===========================================================================
XGameResult XTaskQueueCreate(
    XTaskQueueDispatchMode workDispatchMode,
    XTaskQueueDispatchMode completionDispatchMode,
    XTaskQueueHandle* queue) {
    if (!queue) {
        return XGR_E_POINTER;
    }
    auto* obj = new (std::nothrow) XTaskQueueObject();
    if (!obj) {
        return XGR_E_OUTOFMEMORY;
    }
    obj->ports[0].mode = workDispatchMode;
    obj->ports[1].mode = completionDispatchMode;
    StartPortWorkerIfNeeded(obj, XTaskQueuePort_Work);
    StartPortWorkerIfNeeded(obj, XTaskQueuePort_Completion);
    RegisterValidHandle(obj);
    *queue = obj;
    return XGR_S_OK;
}

XGameResult XTaskQueueDuplicateHandle(
    XTaskQueueHandle queueHandle,
    XTaskQueueHandle* duplicatedHandle) {
    if (!queueHandle || !duplicatedHandle) {
        return XGR_E_INVALIDARG;
    }
    queueHandle->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = queueHandle;
    return XGR_S_OK;
}

void XTaskQueueCloseHandle(XTaskQueueHandle queue) {
    if (!queue) {
        return;
    }
    if (queue->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        TerminateQueueInternal(queue, true);
        UnregisterValidHandle(queue);
        delete queue;
    }
}

XGameResult XTaskQueueSubmitDelayedCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t delayMs,
    void* callbackContext,
    XTaskQueueCallback* callback) {
    if (!queue || !callback) {
        return XGR_E_INVALIDARG;
    }
    const size_t idx = static_cast<size_t>(port);
    if (idx > 1) {
        return XGR_E_INVALIDARG;
    }

    if (queue->ports[idx].mode == XTaskQueueDispatchMode_Immediate && delayMs == 0) {
        bool canceled = false;
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            canceled = queue->terminating;
        }
        callback(callbackContext, canceled);
        return XGR_S_OK;
    }

    {
        std::lock_guard<std::mutex> lock(queue->mutex);
        if (queue->terminating) {
            return XGR_E_ABORT;
        }
        TaskItem item;
        item.callback = callback;
        item.context = callbackContext;
        item.ready_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
        queue->ports[idx].items.push_back(item);
    }
    queue->cv.notify_all();
    NotifyMonitors(queue, port);
    return XGR_S_OK;
}

XGameResult XTaskQueueSubmitCallback(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    void* callbackContext,
    XTaskQueueCallback* callback) {
    return XTaskQueueSubmitDelayedCallback(queue, port, 0, callbackContext, callback);
}

bool XTaskQueueDispatch(
    XTaskQueueHandle queue,
    XTaskQueuePort port,
    uint32_t timeoutInMs) {
    if (!queue) {
        return false;
    }
    const size_t idx = static_cast<size_t>(port);
    if (idx > 1) {
        return false;
    }

    const auto deadline =
        (timeoutInMs == 0xFFFFFFFFu)
            ? std::chrono::steady_clock::time_point::max()
            : (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutInMs));

    TaskItem item;
    bool canceled = false;
    {
        std::unique_lock<std::mutex> lock(queue->mutex);
        for (;;) {
            std::chrono::steady_clock::time_point next_ready;
            if (PopReadyItemLocked(queue->ports[idx], item, next_ready)) {
                canceled = queue->terminating;
                break;
            }
            if (queue->terminating) {
                return false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            auto wait_until = deadline;
            if (!queue->ports[idx].items.empty() && next_ready < wait_until) {
                wait_until = next_ready;
            }
            if (wait_until == std::chrono::steady_clock::time_point::max()) {
                queue->cv.wait(lock);
            } else {
                queue->cv.wait_until(lock, wait_until);
            }
        }
    }

    if (item.callback) {
        item.callback(item.context, canceled);
    }
    return true;
}

XGameResult XTaskQueueTerminate(
    XTaskQueueHandle queue,
    bool wait,
    void* callbackContext,
    XTaskQueueTerminatedCallback* callback) {
    if (!queue) {
        return XGR_E_INVALIDARG;
    }
    if (callback) {
        std::lock_guard<std::mutex> lock(queue->mutex);
        queue->terminated_callbacks.emplace_back(callback, callbackContext);
    }
    TerminateQueueInternal(queue, wait);
    if (!wait && callback) {
        // If not waiting, invoke callback once workers are signaled if none exist.
        bool any_worker = false;
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            any_worker = queue->ports[0].has_worker || queue->ports[1].has_worker;
        }
        if (!any_worker) {
            std::vector<std::pair<XTaskQueueTerminatedCallback*, void*>> cbs;
            {
                std::lock_guard<std::mutex> lock(queue->mutex);
                cbs.swap(queue->terminated_callbacks);
            }
            for (const auto& cb : cbs) {
                if (cb.first) {
                    cb.first(cb.second);
                }
            }
        }
    }
    return XGR_S_OK;
}

XGameResult XTaskQueueGetCurrentProcessTaskQueue(XTaskQueueHandle* queue) {
    if (!queue) {
        return XGR_E_POINTER;
    }
    XTaskQueueHandle q = EnsureDefaultProcessQueue();
    if (!q) {
        *queue = nullptr;
        return XGR_E_FAIL;
    }
    return XTaskQueueDuplicateHandle(q, queue);
}

void XTaskQueueSetCurrentProcessTaskQueue(XTaskQueueHandle queue) {
    XTaskQueueHandle old_q = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_process_queue_mutex);
        old_q = g_process_queue;
        g_process_queue = nullptr;
        if (queue) {
            XTaskQueueDuplicateHandle(queue, &g_process_queue);
        }
    }
    if (old_q) {
        XTaskQueueCloseHandle(old_q);
    }
}

XGameResult XTaskQueueRegisterMonitor(
    XTaskQueueHandle queue,
    void* callbackContext,
    XTaskQueueMonitorCallback* callback,
    XTaskQueueRegistrationToken* token) {
    if (!queue || !callback || !token) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(queue->mutex);
    const uint64_t id = queue->next_monitor_token++;
    queue->monitors.push_back(MonitorEntry{id, callback, callbackContext});
    token->token = id;
    return XGR_S_OK;
}

void XTaskQueueUnregisterMonitor(
    XTaskQueueHandle queue,
    XTaskQueueRegistrationToken token) {
    if (!queue) {
        return;
    }
    std::lock_guard<std::mutex> lock(queue->mutex);
    queue->monitors.erase(
        std::remove_if(queue->monitors.begin(), queue->monitors.end(),
                       [&](const MonitorEntry& e) { return e.token == token.token; }),
        queue->monitors.end());
}

// ===========================================================================
// XAsync.h
// ===========================================================================
XGameResult XAsyncGetStatus(XAsyncBlock* asyncBlock, bool wait) {
    auto state = GetAsyncState(asyncBlock);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (wait && !state->completed) {
        state->cv.wait(lock, [&]() { return state->completed; });
    }
    return state->status;
}

XGameResult XAsyncGetResultSize(XAsyncBlock* asyncBlock, size_t* bufferSize) {
    if (!bufferSize) {
        return XGR_E_POINTER;
    }
    auto state = GetAsyncState(asyncBlock);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->completed) {
        return XGR_E_PENDING;
    }
    *bufferSize = state->result_size;
    return state->status;
}

void XAsyncCancel(XAsyncBlock* asyncBlock) {
    auto state = GetAsyncState(asyncBlock);
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->completed) {
        state->canceled = true;
        state->status = XGR_E_ABORT;
        state->completed = true;
        state->cv.notify_all();
    }
}

XGameResult XAsyncRun(XAsyncBlock* asyncBlock, XAsyncWork* work) {
    if (!asyncBlock || !work) {
        return XGR_E_INVALIDARG;
    }
    BeginAsyncOperation(asyncBlock, [asyncBlock, work](XAsyncState* st) {
        XGameResult hr = work(asyncBlock);
        CompleteAsyncState(st, hr, 0);
    });
    return XGR_S_OK;
}

// ===========================================================================
// XSystem.h
// ===========================================================================
uint32_t XSystemGetDeviceType() {
    return static_cast<uint32_t>(scarlettx::XGameRuntimeHle::Instance().GetDeviceType());
}

XSystemAnalyticsInfo XSystemGetAnalyticsInfo() {
    XSystemAnalyticsInfo info{};
    info.osVersion.major = 10;
    info.osVersion.minor = 0;
    info.osVersion.build = 26100;
    info.osVersion.revision = 2499;
    info.hostingOsVersion = info.osVersion;
    std::strncpy(info.family, "Xbox", sizeof(info.family) - 1);
    std::strncpy(info.form, "Xbox Series X", sizeof(info.form) - 1);
    return info;
}

XSystemRuntimeInfo XSystemGetRuntimeInfo() {
    XSystemRuntimeInfo info{};
    info.runtimeVersion.major = 10;
    info.runtimeVersion.minor = 0;
    info.runtimeVersion.build = 26100;
    info.runtimeVersion.revision = 2499;
    info.availableVersion = info.runtimeVersion;
    return info;
}

XGameResult XSystemGetAppSpecificDeviceId(
    size_t appSpecificDeviceIdSize,
    char* appSpecificDeviceId,
    size_t* appSpecificDeviceIdUsed) {
    return CopyStringToBuffer(
        "SCARLETTX-DEV-00000001-ANACONDA",
        appSpecificDeviceIdSize,
        appSpecificDeviceId,
        appSpecificDeviceIdUsed);
}

XGameResult XSystemGetConsoleId(
    size_t consoleIdSize,
    char* consoleId,
    size_t* consoleIdUsed) {
    return CopyStringToBuffer(
        "SCARLETTX.CONSOLE.0001.0000",
        consoleIdSize,
        consoleId,
        consoleIdUsed);
}

XGameResult XSystemGetXboxLiveSandboxId(
    size_t sandboxIdSize,
    char* sandboxId,
    size_t* sandboxIdUsed) {
    return CopyStringToBuffer(
        "RETAIL",
        sandboxIdSize,
        sandboxId,
        sandboxIdUsed);
}

bool XSystemIsHandleValid(void* handle) {
    return IsRegisteredHandle(handle);
}

// ===========================================================================
// XUser.h
// ===========================================================================
XGameResult XUserAddAsync(XUserAddOptions /*options*/, XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    BeginAsyncOperation(async, [](XAsyncState* st) {
        auto* user = new (std::nothrow) XUser();
        if (!user) {
            CompleteAsyncState(st, XGR_E_OUTOFMEMORY, 0);
            return;
        }
        scarlettx::XUserProfile profile;
        if (scarlettx::XGameRuntimeHle::Instance().GetDefaultUser(profile)) {
            user->xuid = profile.xuid;
            user->gamertag_classic = profile.gamertag;
        }
        RegisterValidHandle(user);
        st->result_handle = user;
        CompleteAsyncState(st, XGR_S_OK, sizeof(XUserHandle));
    });
    return XGR_S_OK;
}

XGameResult XUserAddResult(XAsyncBlock* async, XUserHandle* newUser) {
    if (!async || !newUser) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *newUser = nullptr;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state || !state->result_handle) {
        *newUser = nullptr;
        return XGR_E_FAIL;
    }
    *newUser = static_cast<XUserHandle>(state->result_handle);
    return XGR_S_OK;
}

XGameResult XUserDuplicateHandle(XUserHandle handle, XUserHandle* duplicatedHandle) {
    if (!handle || !duplicatedHandle) {
        return XGR_E_INVALIDARG;
    }
    handle->ref_count.fetch_add(1, std::memory_order_relaxed);
    *duplicatedHandle = handle;
    return XGR_S_OK;
}

void XUserCloseHandle(XUserHandle user) {
    if (!user) {
        return;
    }
    if (user->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        UnregisterValidHandle(user);
        delete user;
    }
}

XGameResult XUserGetId(XUserHandle user, uint64_t* userId) {
    if (!user || !userId) {
        return XGR_E_INVALIDARG;
    }
    *userId = user->xuid;
    return XGR_S_OK;
}

XGameResult XUserGetLocalId(XUserHandle user, XUserLocalId* userLocalId) {
    if (!user || !userLocalId) {
        return XGR_E_INVALIDARG;
    }
    userLocalId->value = user->local_id;
    return XGR_S_OK;
}

XGameResult XUserGetGamertag(
    XUserHandle user,
    XUserGamertagComponent gamertagComponent,
    size_t gamertagSize,
    char* gamertag,
    size_t* gamertagUsed) {
    if (!user) {
        return XGR_E_INVALIDARG;
    }
    const std::string* src = &user->gamertag_classic;
    switch (gamertagComponent) {
        case XUserGamertagComponent_Classic:
            src = &user->gamertag_classic;
            break;
        case XUserGamertagComponent_Modern:
            src = &user->gamertag_modern;
            break;
        case XUserGamertagComponent_ModernSuffix:
            src = &user->gamertag_suffix;
            break;
        case XUserGamertagComponent_UniqueModern:
            src = &user->gamertag_unique;
            break;
        default:
            return XGR_E_INVALIDARG;
    }
    return CopyStringToBuffer(*src, gamertagSize, gamertag, gamertagUsed);
}

XGameResult XUserGetState(XUserHandle user, XUserState* state) {
    if (!user || !state) {
        return XGR_E_INVALIDARG;
    }
    *state = user->state;
    return XGR_S_OK;
}

XGameResult XUserGetAgeGroup(XUserHandle user, XUserAgeGroup* ageGroup) {
    if (!user || !ageGroup) {
        return XGR_E_INVALIDARG;
    }
    *ageGroup = user->age_group;
    return XGR_S_OK;
}

XGameResult XUserGetIsGuest(XUserHandle user, bool* isGuest) {
    if (!user || !isGuest) {
        return XGR_E_INVALIDARG;
    }
    *isGuest = user->is_guest;
    return XGR_S_OK;
}

XGameResult XUserGetMaxUsers(uint32_t* maxUsers) {
    if (!maxUsers) {
        return XGR_E_POINTER;
    }
    *maxUsers = 16;
    return XGR_S_OK;
}

XGameResult XUserCheckPrivilege(
    XUserHandle user,
    XUserPrivilegeOptions /*options*/,
    XUserPrivilege /*privilege*/,
    bool* hasPrivilege,
    XUserPrivilegeDenyReason* reason) {
    if (!user || !hasPrivilege) {
        return XGR_E_INVALIDARG;
    }
    *hasPrivilege = true;
    if (reason) {
        *reason = XUserPrivilegeDenyReason_None;
    }
    return XGR_S_OK;
}

XGameResult XUserGetDefaultXuid(uint64_t* out_xuid) {
    if (!out_xuid) {
        return XGR_E_INVALIDARG;
    }
    scarlettx::XUserProfile user;
    if (!scarlettx::XGameRuntimeHle::Instance().GetDefaultUser(user)) {
        return XGR_E_GAMERUNTIME_NOT_INITIALIZED;
    }
    *out_xuid = user.xuid;
    return XGR_S_OK;
}

// ===========================================================================
// XGameSave.h
// ===========================================================================
XGameResult XGameSaveInitializeProvider(
    XUserHandle requestingUser,
    const char* configurationId,
    bool /*syncOnDemand*/,
    XGameSaveProviderHandle* provider) {
    if (!requestingUser || !configurationId || !*configurationId || !provider) {
        return XGR_E_INVALIDARG;
    }
    auto* p = new (std::nothrow) XGameSaveProvider();
    if (!p) {
        return XGR_E_OUTOFMEMORY;
    }
    p->config_id = configurationId;
    p->root_dir = ResolveSaveRoot(p->config_id);
    std::error_code ec;
    fs::create_directories(p->root_dir, ec);
    if (ec) {
        delete p;
        return XGR_E_FAIL;
    }
    RegisterValidHandle(p);
    *provider = p;
    return XGR_S_OK;
}

XGameResult XGameSaveInitializeProviderAsync(
    XUserHandle requestingUser,
    const char* configurationId,
    bool syncOnDemand,
    XAsyncBlock* async) {
    if (!requestingUser || !configurationId || !async) {
        return XGR_E_INVALIDARG;
    }
    std::string cfg = configurationId;
    BeginAsyncOperation(async, [requestingUser, cfg, syncOnDemand](XAsyncState* st) {
        XGameSaveProviderHandle prov = nullptr;
        XGameResult hr = XGameSaveInitializeProvider(
            requestingUser, cfg.c_str(), syncOnDemand, &prov);
        st->result_handle = prov;
        CompleteAsyncState(st, hr, sizeof(XGameSaveProviderHandle));
    });
    return XGR_S_OK;
}

XGameResult XGameSaveInitializeProviderResult(
    XAsyncBlock* async,
    XGameSaveProviderHandle* provider) {
    if (!async || !provider) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        *provider = nullptr;
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state || !state->result_handle) {
        *provider = nullptr;
        return XGR_E_FAIL;
    }
    *provider = static_cast<XGameSaveProviderHandle>(state->result_handle);
    return XGR_S_OK;
}

void XGameSaveCloseProvider(XGameSaveProviderHandle provider) {
    if (!provider) {
        return;
    }
    if (provider->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        UnregisterValidHandle(provider);
        delete provider;
    }
}

XGameResult XGameSaveGetRemainingQuota(
    XGameSaveProviderHandle provider,
    int64_t* remainingQuota) {
    if (!provider || !remainingQuota) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(provider->mutex);
    const uint64_t used = ComputeDirectorySize(provider->root_dir);
    *remainingQuota = (used >= provider->max_quota_bytes)
                          ? 0
                          : static_cast<int64_t>(provider->max_quota_bytes - used);
    return XGR_S_OK;
}

XGameResult XGameSaveCreateContainer(
    XGameSaveProviderHandle provider,
    const char* name,
    XGameSaveContainerHandle* containerContext) {
    if (!provider || !name || !*name || !containerContext) {
        return XGR_E_INVALIDARG;
    }
    auto* c = new (std::nothrow) XGameSaveContainer();
    if (!c) {
        return XGR_E_OUTOFMEMORY;
    }
    provider->ref_count.fetch_add(1, std::memory_order_relaxed);
    c->provider = provider;
    c->name = name;
    c->dir_path = provider->root_dir / SanitizeSaveName(name);
    std::error_code ec;
    fs::create_directories(c->dir_path, ec);
    RegisterValidHandle(c);
    *containerContext = c;
    return XGR_S_OK;
}

void XGameSaveCloseContainer(XGameSaveContainerHandle context) {
    if (!context) {
        return;
    }
    XGameSaveProviderHandle prov = context->provider;
    UnregisterValidHandle(context);
    delete context;
    if (prov) {
        XGameSaveCloseProvider(prov);
    }
}

XGameResult XGameSaveDeleteContainer(
    XGameSaveProviderHandle provider,
    const char* name) {
    if (!provider || !name || !*name) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(provider->mutex);
    std::error_code ec;
    fs::remove_all(provider->root_dir / SanitizeSaveName(name), ec);
    return ec ? XGR_E_FAIL : XGR_S_OK;
}

XGameResult XGameSaveEnumerateContainerInfo(
    XGameSaveProviderHandle provider,
    void* context,
    XGameSaveContainerInfoCallback* callback) {
    if (!provider || !callback) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(provider->mutex);
    std::error_code ec;
    if (!fs::exists(provider->root_dir, ec)) {
        return XGR_S_OK;
    }
    for (const auto& entry : fs::directory_iterator(provider->root_dir, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string cname = entry.path().filename().string();
        uint32_t blob_count = 0;
        uint64_t total_size = 0;
        for (const auto& f : fs::directory_iterator(entry.path(), ec)) {
            if (f.is_regular_file(ec)) {
                ++blob_count;
                total_size += f.file_size(ec);
            }
        }
        XGameSaveContainerInfo info{};
        info.name = cname.c_str();
        info.displayName = cname.c_str();
        info.blobCount = blob_count;
        info.totalSize = total_size;
        info.lastModifiedTime = 0;
        info.needsSync = false;
        if (!callback(&info, context)) {
            break;
        }
    }
    return XGR_S_OK;
}

XGameResult XGameSaveCreateUpdate(
    XGameSaveContainerHandle container,
    const char* displayName,
    XGameSaveUpdateHandle* updateContext) {
    if (!container || !updateContext) {
        return XGR_E_INVALIDARG;
    }
    auto* u = new (std::nothrow) XGameSaveUpdate();
    if (!u) {
        return XGR_E_OUTOFMEMORY;
    }
    u->container = container;
    u->display_name = displayName ? displayName : container->name;
    RegisterValidHandle(u);
    *updateContext = u;
    return XGR_S_OK;
}

void XGameSaveCloseUpdate(XGameSaveUpdateHandle context) {
    if (!context) {
        return;
    }
    UnregisterValidHandle(context);
    delete context;
}

XGameResult XGameSaveSubmitBlobWrite(
    XGameSaveUpdateHandle updateContext,
    const char* blobName,
    const uint8_t* data,
    size_t byteCount) {
    if (!updateContext || !blobName || !*blobName || (!data && byteCount > 0)) {
        return XGR_E_INVALIDARG;
    }
    std::vector<uint8_t> copy;
    if (byteCount > 0) {
        copy.assign(data, data + byteCount);
    }
    updateContext->pending_writes[blobName] = std::move(copy);
    return XGR_S_OK;
}

XGameResult XGameSaveSubmitBlobDelete(
    XGameSaveUpdateHandle updateContext,
    const char* blobName) {
    if (!updateContext || !blobName || !*blobName) {
        return XGR_E_INVALIDARG;
    }
    updateContext->pending_writes.erase(blobName);
    updateContext->pending_deletes.emplace_back(blobName);
    return XGR_S_OK;
}

XGameResult XGameSaveSubmitUpdate(XGameSaveUpdateHandle updateContext) {
    if (!updateContext || !updateContext->container || !updateContext->container->provider) {
        return XGR_E_INVALIDARG;
    }
    auto* provider = updateContext->container->provider;
    std::lock_guard<std::mutex> lock(provider->mutex);

    std::error_code ec;
    fs::create_directories(updateContext->container->dir_path, ec);

    for (const auto& del_name : updateContext->pending_deletes) {
        fs::remove(updateContext->container->dir_path / SanitizeSaveName(del_name), ec);
    }
    for (const auto& [blob_name, bytes] : updateContext->pending_writes) {
        fs::path file_path = updateContext->container->dir_path / SanitizeSaveName(blob_name);
        std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            return XGR_E_FAIL;
        }
        if (!bytes.empty()) {
            ofs.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        if (!ofs.good()) {
            return XGR_E_FAIL;
        }
    }
    return XGR_S_OK;
}

XGameResult XGameSaveSubmitUpdateAsync(
    XGameSaveUpdateHandle updateContext,
    XAsyncBlock* async) {
    if (!updateContext || !async) {
        return XGR_E_INVALIDARG;
    }
    BeginAsyncOperation(async, [updateContext](XAsyncState* st) {
        XGameResult hr = XGameSaveSubmitUpdate(updateContext);
        CompleteAsyncState(st, hr, 0);
    });
    return XGR_S_OK;
}

XGameResult XGameSaveSubmitUpdateResult(XAsyncBlock* async) {
    if (!async) {
        return XGR_E_INVALIDARG;
    }
    return XAsyncGetStatus(async, true);
}

XGameResult XGameSaveReadBlobData(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t* countOfBlobs,
    size_t blobsSize,
    XGameSaveBlob* blobData) {
    if (!container || !blobNames || !countOfBlobs || !blobData) {
        return XGR_E_INVALIDARG;
    }
    auto* provider = container->provider;
    std::lock_guard<std::mutex> lock(provider->mutex);

    const uint32_t requested = *countOfBlobs;
    const size_t headers_bytes = sizeof(XGameSaveBlob) * requested;
    if (blobsSize < headers_bytes) {
        return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }

    uint8_t* base = reinterpret_cast<uint8_t*>(blobData);
    size_t cursor = headers_bytes;
    uint32_t read_count = 0;

    for (uint32_t i = 0; i < requested; ++i) {
        if (!blobNames[i]) {
            return XGR_E_INVALIDARG;
        }
        const std::string name = blobNames[i];
        fs::path file_path = container->dir_path / SanitizeSaveName(name);
        std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
        if (!ifs) {
            return XGR_E_GS_BLOB_NOT_FOUND;
        }
        const std::streamsize fsize = ifs.tellg();
        ifs.seekg(0, std::ios::beg);

        const size_t name_len = name.size() + 1;
        const size_t data_len = static_cast<size_t>(fsize > 0 ? fsize : 0);
        if (cursor + name_len + data_len > blobsSize) {
            return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
        }

        char* name_dst = reinterpret_cast<char*>(base + cursor);
        std::memcpy(name_dst, name.c_str(), name_len);
        cursor += name_len;

        uint8_t* data_dst = base + cursor;
        if (data_len > 0) {
            ifs.read(reinterpret_cast<char*>(data_dst), fsize);
        }
        cursor += data_len;

        blobData[i].info.name = name_dst;
        blobData[i].info.size = static_cast<uint32_t>(data_len);
        blobData[i].data = data_dst;
        ++read_count;
    }

    *countOfBlobs = read_count;
    return XGR_S_OK;
}

XGameResult XGameSaveReadBlobDataAsync(
    XGameSaveContainerHandle container,
    const char** blobNames,
    uint32_t countOfBlobs,
    XAsyncBlock* async) {
    if (!container || !blobNames || !async) {
        return XGR_E_INVALIDARG;
    }
    std::vector<std::string> names;
    names.reserve(countOfBlobs);
    for (uint32_t i = 0; i < countOfBlobs; ++i) {
        if (!blobNames[i]) {
            return XGR_E_INVALIDARG;
        }
        names.emplace_back(blobNames[i]);
    }

    BeginAsyncOperation(async, [container, names = std::move(names)](XAsyncState* st) {
        auto* provider = container->provider;
        std::lock_guard<std::mutex> lock(provider->mutex);

        // Compute packed payload size: [uint32_t count] + per-blob (name_len, data_len, name, data)
        // Also compute GDK caller buffer requirement for XGameSaveReadBlobDataResult.
        size_t required_gdk_size = sizeof(XGameSaveBlob) * names.size();
        std::vector<std::pair<std::string, std::vector<uint8_t>>> blobs;
        blobs.reserve(names.size());

        for (const auto& name : names) {
            fs::path file_path = container->dir_path / SanitizeSaveName(name);
            std::ifstream ifs(file_path, std::ios::binary | std::ios::ate);
            if (!ifs) {
                CompleteAsyncState(st, XGR_E_GS_BLOB_NOT_FOUND, 0);
                return;
            }
            const std::streamsize fsize = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            std::vector<uint8_t> bytes(static_cast<size_t>(fsize > 0 ? fsize : 0));
            if (!bytes.empty()) {
                ifs.read(reinterpret_cast<char*>(bytes.data()), fsize);
            }
            required_gdk_size += (name.size() + 1) + bytes.size();
            blobs.emplace_back(name, std::move(bytes));
        }

        // Serialize into st->result_payload for XGameSaveReadBlobDataResult.
        std::vector<uint8_t> packed;
        auto append = [&](const void* src, size_t n) {
            const auto* p = static_cast<const uint8_t*>(src);
            packed.insert(packed.end(), p, p + n);
        };
        uint32_t cnt = static_cast<uint32_t>(blobs.size());
        append(&cnt, sizeof(cnt));
        for (const auto& [name, bytes] : blobs) {
            uint32_t nlen = static_cast<uint32_t>(name.size() + 1);
            uint32_t dlen = static_cast<uint32_t>(bytes.size());
            append(&nlen, sizeof(nlen));
            append(&dlen, sizeof(dlen));
            append(name.c_str(), nlen);
            if (dlen > 0) {
                append(bytes.data(), dlen);
            }
        }
        st->result_payload = std::move(packed);
        CompleteAsyncState(st, XGR_S_OK, required_gdk_size);
    });
    return XGR_S_OK;
}

XGameResult XGameSaveReadBlobDataResult(
    XAsyncBlock* async,
    size_t blobsSize,
    XGameSaveBlob* blobData,
    uint32_t* countOfBlobs) {
    if (!async || !blobData || !countOfBlobs) {
        return XGR_E_INVALIDARG;
    }
    XGameResult hr = XAsyncGetStatus(async, true);
    if (hr != XGR_S_OK) {
        return hr;
    }
    auto state = GetAsyncState(async);
    if (!state) {
        return XGR_E_INVALIDARG;
    }
    if (blobsSize < state->result_size) {
        return XGR_E_GS_PROVIDED_BUFFER_TOO_SMALL;
    }

    const auto& packed = state->result_payload;
    if (packed.size() < sizeof(uint32_t)) {
        return XGR_E_FAIL;
    }
    size_t p_off = 0;
    uint32_t cnt = 0;
    std::memcpy(&cnt, packed.data() + p_off, sizeof(cnt));
    p_off += sizeof(cnt);

    uint8_t* base = reinterpret_cast<uint8_t*>(blobData);
    size_t cursor = sizeof(XGameSaveBlob) * cnt;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t nlen = 0;
        uint32_t dlen = 0;
        std::memcpy(&nlen, packed.data() + p_off, sizeof(nlen));
        p_off += sizeof(nlen);
        std::memcpy(&dlen, packed.data() + p_off, sizeof(dlen));
        p_off += sizeof(dlen);

        char* name_dst = reinterpret_cast<char*>(base + cursor);
        std::memcpy(name_dst, packed.data() + p_off, nlen);
        p_off += nlen;
        cursor += nlen;

        uint8_t* data_dst = base + cursor;
        if (dlen > 0) {
            std::memcpy(data_dst, packed.data() + p_off, dlen);
            p_off += dlen;
            cursor += dlen;
        }

        blobData[i].info.name = name_dst;
        blobData[i].info.size = dlen;
        blobData[i].data = data_dst;
    }

    *countOfBlobs = cnt;
    return XGR_S_OK;
}

XGameResult XGameSaveEnumerateBlobInfo(
    XGameSaveContainerHandle container,
    void* context,
    XGameSaveBlobInfoCallback* callback) {
    if (!container || !callback) {
        return XGR_E_INVALIDARG;
    }
    auto* provider = container->provider;
    std::lock_guard<std::mutex> lock(provider->mutex);
    std::error_code ec;
    if (!fs::exists(container->dir_path, ec)) {
        return XGR_S_OK;
    }
    for (const auto& entry : fs::directory_iterator(container->dir_path, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::string bname = entry.path().filename().string();
        XGameSaveBlobInfo info{};
        info.name = bname.c_str();
        info.size = static_cast<uint32_t>(entry.file_size(ec));
        if (!callback(&info, context)) {
            break;
        }
    }
    return XGR_S_OK;
}

// ===========================================================================
// XMem.h
// ===========================================================================
void* XMemAllocDefault(size_t dwSize, uint64_t /*dwAttributes*/) {
    const size_t sz = dwSize == 0 ? 16 : dwSize;
    void* ptr = std::calloc(1, sz);
    return ptr;
}

void XMemFreeDefault(void* lpAddress, uint64_t /*dwAttributes*/) {
    std::free(lpAddress);
}

void* XMemAlloc(size_t dwSize, uint64_t dwAttributes) {
    XMemAllocFunction* hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
        hook = g_mem_alloc_hook;
    }
    if (hook) {
        return hook(dwSize, dwAttributes);
    }
    return XMemAllocDefault(dwSize, dwAttributes);
}

void XMemFree(void* lpAddress, uint64_t dwAttributes) {
    XMemFreeFunction* hook = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
        hook = g_mem_free_hook;
    }
    if (hook) {
        hook(lpAddress, dwAttributes);
        return;
    }
    XMemFreeDefault(lpAddress, dwAttributes);
}

XGameResult XMemSetAllocationHooks(
    XMemAllocFunction* allocFunc,
    XMemFreeFunction* freeFunc) {
    if ((allocFunc == nullptr) != (freeFunc == nullptr)) {
        return XGR_E_INVALIDARG;
    }
    std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
    g_mem_alloc_hook = allocFunc;
    g_mem_free_hook = freeFunc;
    return XGR_S_OK;
}

XGameResult XMemGetAllocationHooks(
    XMemAllocFunction** allocFunc,
    XMemFreeFunction** freeFunc) {
    if (!allocFunc || !freeFunc) {
        return XGR_E_POINTER;
    }
    std::lock_guard<std::mutex> lock(g_mem_hooks_mutex);
    *allocFunc = g_mem_alloc_hook ? g_mem_alloc_hook : &XMemAllocDefault;
    *freeFunc = g_mem_free_hook ? g_mem_free_hook : &XMemFreeDefault;
    return XGR_S_OK;
}

void* XMemVirtualAlloc(
    void* baseAddress,
    size_t size,
    uint32_t allocationType,
    uint32_t protect) {
#if defined(_WIN32)
    DWORD flAlloc = allocationType ? static_cast<DWORD>(allocationType)
                                   : (MEM_RESERVE | MEM_COMMIT);
    DWORD flProtect = protect ? static_cast<DWORD>(protect) : PAGE_READWRITE;
    return VirtualAlloc(baseAddress, size, flAlloc, flProtect);
#else
    (void)baseAddress;
    (void)allocationType;
    (void)protect;
    return std::calloc(1, size == 0 ? 4096 : size);
#endif
}

bool XMemVirtualFree(void* address, size_t size, uint32_t freeType) {
    if (!address) {
        return false;
    }
#if defined(_WIN32)
    DWORD flFree = freeType ? static_cast<DWORD>(freeType) : MEM_RELEASE;
    return VirtualFree(address, flFree == MEM_RELEASE ? 0 : size, flFree) != 0;
#else
    (void)size;
    (void)freeType;
    std::free(address);
    return true;
#endif
}

// ===========================================================================
// XThread.h & XGame.h
// ===========================================================================
bool XThreadIsTimeSensitive() {
    return g_thread_time_sensitive;
}

XGameResult XThreadSetTimeSensitive(bool isTimeSensitive) {
    g_thread_time_sensitive = isTimeSensitive;
    return XGR_S_OK;
}

void XThreadAssertNotTimeSensitive() {
    // Non-fatal in HLE; real console triggers a debug break if time-sensitive.
}

XGameResult XGameGetXboxTitleId(uint32_t* titleId) {
    if (!titleId) {
        return XGR_E_POINTER;
    }
    if (const char* env_tid = std::getenv("SCARLETTX_TITLE_ID"); env_tid && *env_tid) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(env_tid, &end, 16);
        if (end && end != env_tid) {
            *titleId = static_cast<uint32_t>(parsed);
            return XGR_S_OK;
        }
    }
    *titleId = 0x5CA00001u;
    return XGR_S_OK;
}

void XErrorReport(XGameResult hr, const char* msg) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] XErrorReport(hr=0x%08X, msg=%s)\n",
                 static_cast<unsigned>(hr), msg ? msg : "<null>");
    std::fflush(stderr);
}

XGameResult InitializeApiImpl(uint32_t build, uint32_t revision) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] InitializeApiImpl(build=%u, rev=%u)\n",
                 build, revision);
    std::fflush(stderr);
    return XGameRuntimeInitialize();
}

XGameResult InitializeApiImplEx(uint32_t build, uint32_t revision, uint64_t flags) {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] InitializeApiImplEx(build=%u, rev=%u, flags=0x%llX)\n",
                 build, revision, static_cast<unsigned long long>(flags));
    std::fflush(stderr);
    return XGameRuntimeInitialize();
}

void UninitializeApiImpl() {
    std::fprintf(stderr, "[ScarlettX][xgameruntime] UninitializeApiImpl()\n");
    std::fflush(stderr);
    XGameRuntimeUninitialize();
}

static void FormatGuid(const void* guid_ptr, char* out, size_t out_size) {
    if (!guid_ptr) {
        std::snprintf(out, out_size, "<null>");
        return;
    }
    const auto* b = reinterpret_cast<const uint8_t*>(guid_ptr);
    const uint32_t d1 = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    const uint16_t d2 = static_cast<uint16_t>(b[4]) | (static_cast<uint16_t>(b[5]) << 8);
    const uint16_t d3 = static_cast<uint16_t>(b[6]) | (static_cast<uint16_t>(b[7]) << 8);
    std::snprintf(out, out_size, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  d1, d2, d3, b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

XGameResult QueryApiImpl(const void* api_id, const void* riid, void** out_impl) {
    if (!api_id || !riid || !out_impl) {
        return XGR_E_POINTER;
    }
    *out_impl = nullptr;
    char api_str[64] = {};
    char iid_str[64] = {};
    FormatGuid(api_id, api_str, sizeof(api_str));
    FormatGuid(riid, iid_str, sizeof(iid_str));
    std::fprintf(stderr,
                 "[ScarlettX][xgameruntime] QueryApiImpl(api=%s, iid=%s) -> E_NOINTERFACE\n",
                 api_str, iid_str);
    std::fflush(stderr);
    // E_NOINTERFACE until specific GDK ApiImpl interface GUIDs are observed at runtime.
    return static_cast<XGameResult>(0x80004002u);
}

}  // extern "C"
