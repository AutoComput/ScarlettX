#define SCARLETTX_BUILDING_XCURL 1
#include "scarlettx/net/xcurl_abi.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct XCurlEasyHandle {
    std::string url;
    std::string method = "GET";
    std::string user_agent;
    std::vector<uint8_t> post_data;
    curl_write_callback write_cb = nullptr;
    void* write_data = nullptr;
    int64_t timeout_sec = 30;
    int64_t response_code = 0;
    int64_t downloaded_bytes = 0;
    std::string content_type;
};

struct XCurlMockRule {
    std::string url_prefix;
    int64_t response_code = 200;
    std::string content_type = "application/json";
    std::vector<uint8_t> body;
};

std::mutex g_mutex;
bool g_global_inited = false;
std::unordered_map<void*, std::unique_ptr<XCurlEasyHandle>> g_handles;
std::vector<XCurlMockRule> g_mocks;

}  // namespace

extern "C" {

CURLcode curl_global_init(int64_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_global_inited = true;
    return CURLE_OK;
}

void curl_global_cleanup(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles.clear();
    g_global_inited = false;
}

CURL* curl_easy_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto h = std::make_unique<XCurlEasyHandle>();
    void* raw = h.get();
    g_handles[raw] = std::move(h);
    return raw;
}

void curl_easy_cleanup(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handles.erase(curl);
}

void curl_easy_reset(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it != g_handles.end()) {
        *it->second = XCurlEasyHandle{};
    }
}

CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
    auto& h = *it->second;

    va_list args;
    va_start(args, option);
    CURLcode rc = CURLE_OK;
    switch (option) {
        case CURLOPT_URL: {
            const char* s = va_arg(args, const char*);
            h.url = s ? s : "";
            break;
        }
        case CURLOPT_CUSTOMREQUEST: {
            const char* s = va_arg(args, const char*);
            if (s) h.method = s;
            break;
        }
        case CURLOPT_USERAGENT: {
            const char* s = va_arg(args, const char*);
            h.user_agent = s ? s : "";
            break;
        }
        case CURLOPT_POST: {
            const long v = va_arg(args, long);
            if (v) h.method = "POST";
            break;
        }
        case CURLOPT_POSTFIELDS: {
            const char* p = va_arg(args, const char*);
            if (p) {
                const size_t len = std::strlen(p);
                h.post_data.assign(reinterpret_cast<const uint8_t*>(p),
                                   reinterpret_cast<const uint8_t*>(p) + len);
            }
            break;
        }
        case CURLOPT_POSTFIELDSIZE: {
            (void)va_arg(args, long);
            break;
        }
        case CURLOPT_WRITEFUNCTION: {
            h.write_cb = va_arg(args, curl_write_callback);
            break;
        }
        case CURLOPT_WRITEDATA: {
            h.write_data = va_arg(args, void*);
            break;
        }
        case CURLOPT_HTTPHEADER: {
            (void)va_arg(args, curl_slist*);
            break;
        }
        case CURLOPT_TIMEOUT: {
            h.timeout_sec = va_arg(args, long);
            break;
        }
        case CURLOPT_FOLLOWLOCATION:
        case CURLOPT_SSL_VERIFYPEER: {
            (void)va_arg(args, long);
            break;
        }
        default:
            break;
    }
    va_end(args);
    return rc;
}

CURLcode curl_easy_perform(CURL* curl) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    curl_write_callback cb = nullptr;
    void* ud = nullptr;
    std::vector<uint8_t> body;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_handles.find(curl);
        if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
        auto& h = *it->second;
        if (h.url.empty()) return CURLE_URL_MALFORMAT;

        bool matched = false;
        for (const auto& rule : g_mocks) {
            if (h.url.rfind(rule.url_prefix, 0) == 0) {
                h.response_code = rule.response_code;
                h.content_type = rule.content_type;
                body = rule.body;
                matched = true;
                break;
            }
        }
        if (!matched) {
            h.response_code = 503;
            h.content_type = "application/json";
            const std::string fallback = "{\"status\":\"offline\",\"emulator\":\"ScarlettX-XCurl\"}";
            body.assign(fallback.begin(), fallback.end());
        }
        h.downloaded_bytes = static_cast<int64_t>(body.size());
        cb = h.write_cb;
        ud = h.write_data;
    }

    if (cb && !body.empty()) {
        const size_t written = cb(reinterpret_cast<char*>(body.data()), 1, body.size(), ud);
        if (written != body.size()) {
            return CURLE_WRITE_ERROR;
        }
    }
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_handles.find(curl);
    if (it == g_handles.end()) return CURLE_BAD_FUNCTION_ARGUMENT;
    const auto& h = *it->second;

    va_list args;
    va_start(args, info);
    CURLcode rc = CURLE_OK;
    switch (info) {
        case CURLINFO_RESPONSE_CODE: {
            long* out = va_arg(args, long*);
            if (out) *out = static_cast<long>(h.response_code);
            break;
        }
        case CURLINFO_SIZE_DOWNLOAD_T: {
            int64_t* out = va_arg(args, int64_t*);
            if (out) *out = h.downloaded_bytes;
            break;
        }
        case CURLINFO_CONTENT_TYPE: {
            const char** out = va_arg(args, const char**);
            if (out) *out = h.content_type.c_str();
            break;
        }
        default:
            rc = CURLE_BAD_FUNCTION_ARGUMENT;
            break;
    }
    va_end(args);
    return rc;
}

const char* curl_easy_strerror(CURLcode code) {
    switch (code) {
        case CURLE_OK: return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
        case CURLE_URL_MALFORMAT: return "URL using bad/illegal format";
        case CURLE_COULDNT_CONNECT: return "Couldn't connect to server";
        case CURLE_WRITE_ERROR: return "Failed writing received data";
        case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
        case CURLE_BAD_FUNCTION_ARGUMENT: return "Bad function argument";
        default: return "Unknown error";
    }
}

curl_slist* curl_slist_append(curl_slist* list, const char* string) {
    if (!string) return list;
    auto* node = static_cast<curl_slist*>(std::malloc(sizeof(curl_slist)));
    if (!node) return nullptr;
    const size_t len = std::strlen(string);
    node->data = static_cast<char*>(std::malloc(len + 1));
    if (!node->data) {
        std::free(node);
        return nullptr;
    }
    std::memcpy(node->data, string, len + 1);
    node->next = nullptr;
    if (!list) return node;
    curl_slist* tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(curl_slist* list) {
    while (list) {
        curl_slist* next = list->next;
        std::free(list->data);
        std::free(list);
        list = next;
    }
}

const char* curl_version(void) {
    return "ScarlettX-XCurl/8.0.0-HLE";
}

CURLcode xcurl_register_mock_response(
    const char* url_prefix,
    int64_t response_code,
    const char* content_type,
    const void* body,
    size_t body_size) {
    if (!url_prefix) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_mutex);
    XCurlMockRule rule{};
    rule.url_prefix = url_prefix;
    rule.response_code = response_code;
    rule.content_type = content_type ? content_type : "application/octet-stream";
    if (body && body_size > 0) {
        const auto* b = static_cast<const uint8_t*>(body);
        rule.body.assign(b, b + body_size);
    }
    g_mocks.push_back(std::move(rule));
    return CURLE_OK;
}

void xcurl_clear_mocks(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_mocks.clear();
}

}  // extern "C"
