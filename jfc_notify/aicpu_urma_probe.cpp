// One-shot diagnostic through the official SecureDma AICPU entry. No URMA calls on the Host.
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#include "runtime/dev.h"
#include "runtime/kernel.h"
#include "runtime/mem.h"
#include "runtime/stream.h"
#include "secure_dma_urma_probe_protocol.h"
#include "secure_dma_urma_probe_abi.h"

#ifdef SECURE_DMA_PROBE_HAVE_URMA_HEADERS
#include <urma_api.h>
namespace probe_abi_check {
namespace abi = secure_dma::urma_probe_abi;
#define CHECK_TYPE(real, mirror) static_assert(sizeof(real) == sizeof(mirror) && alignof(real) == alignof(mirror), #real " ABI mismatch")
#define CHECK_FIELD(real, member, mirror, field) \
    static_assert(offsetof(real, member) == offsetof(mirror, field) && \
        sizeof(((real *)nullptr)->member) == sizeof(((mirror *)nullptr)->field), #real "." #member " ABI mismatch")
CHECK_TYPE(urma_eid_t, abi::Eid);
CHECK_TYPE(urma_eid_info_t, abi::EidInfo);
CHECK_FIELD(urma_eid_info_t, eid, abi::EidInfo, eid);
CHECK_FIELD(urma_eid_info_t, eid_index, abi::EidInfo, index);
CHECK_TYPE(urma_jfc_cfg_t, abi::JfcConfig);
CHECK_FIELD(urma_jfc_cfg_t, depth, abi::JfcConfig, depth);
CHECK_FIELD(urma_jfc_cfg_t, flag, abi::JfcConfig, flags);
CHECK_FIELD(urma_jfc_cfg_t, ceqn, abi::JfcConfig, ceqn);
CHECK_FIELD(urma_jfc_cfg_t, jfce, abi::JfcConfig, event);
CHECK_FIELD(urma_jfc_cfg_t, user_ctx, abi::JfcConfig, userContext);
CHECK_TYPE(urma_jfs_cfg_t, abi::JfsConfig);
CHECK_FIELD(urma_jfs_cfg_t, depth, abi::JfsConfig, depth);
CHECK_FIELD(urma_jfs_cfg_t, flag, abi::JfsConfig, flags);
CHECK_FIELD(urma_jfs_cfg_t, trans_mode, abi::JfsConfig, mode);
CHECK_FIELD(urma_jfs_cfg_t, priority, abi::JfsConfig, priority);
CHECK_FIELD(urma_jfs_cfg_t, max_sge, abi::JfsConfig, maxSge);
CHECK_FIELD(urma_jfs_cfg_t, max_rsge, abi::JfsConfig, maxRemoteSge);
CHECK_FIELD(urma_jfs_cfg_t, max_inline_data, abi::JfsConfig, maxInline);
CHECK_FIELD(urma_jfs_cfg_t, rnr_retry, abi::JfsConfig, rnrRetry);
CHECK_FIELD(urma_jfs_cfg_t, err_timeout, abi::JfsConfig, errorTimeout);
CHECK_FIELD(urma_jfs_cfg_t, jfc, abi::JfsConfig, completion);
CHECK_FIELD(urma_jfs_cfg_t, user_ctx, abi::JfsConfig, userContext);
CHECK_TYPE(urma_cr_t, abi::Completion);
static_assert(offsetof(urma_device_t, name) == abi::kDeviceNameOffset &&
    sizeof(((urma_device_t *)nullptr)->name) == abi::kNameBytes, "URMA device name ABI mismatch");
static_assert(offsetof(urma_device_t, type) == abi::kDeviceTypeOffset &&
    sizeof(urma_transport_type_t) == sizeof(int32_t), "URMA device type ABI mismatch");
static_assert(sizeof(urma_status_t) == sizeof(int32_t) && URMA_TRANSPORT_UB == abi::kTransportUb &&
    URMA_TM_RM == abi::kReliableMessage, "URMA enum ABI mismatch");
#undef CHECK_TYPE
#undef CHECK_FIELD
} // namespace probe_abi_check
#endif

namespace {
using namespace secure_dma;

bool Step(const char *name, rtError_t result)
{
    std::printf("step=%s %s code=%d\n", name, result == RT_ERROR_NONE ? "OK" : "FAIL", result);
    return result == RT_ERROR_NONE;
}

bool Number(const char *text, long min, long max, int32_t &out)
{
    if (*text < '0' || *text > '9') { return false; }
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max) { return false; }
    out = static_cast<int32_t>(value);
    return true;
}

void Put32(std::vector<uint8_t> &bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) { bytes.push_back(static_cast<uint8_t>(value >> (8U * i))); }
}

// Matches iter-007 Dispatch::Launch: packed 20-byte AicpuParamHead, zero IO addresses,
// then a protobuf NodeDef with op=SecureDma and bytes attr scc_dma_args=(anchor, trace=0).
std::vector<uint8_t> Arguments(uint64_t anchor)
{
    const char op[] = "SecureDma";
    const char key[] = "scc_dma_args";
    std::vector<uint8_t> entry{0x0a, sizeof(key) - 1U};
    entry.insert(entry.end(), key, key + sizeof(key) - 1U);
    entry.insert(entry.end(), {0x12, 18, 0x12, 16});
    for (unsigned i = 0; i < 8; ++i) { entry.push_back(static_cast<uint8_t>(anchor >> (8U * i))); }
    entry.insert(entry.end(), 8U, 0U);
    std::vector<uint8_t> node{0x12, sizeof(op) - 1U};
    node.insert(node.end(), op, op + sizeof(op) - 1U);
    node.insert(node.end(), {0x1a, static_cast<uint8_t>(entry.size())});
    node.insert(node.end(), entry.begin(), entry.end());
    std::vector<uint8_t> args;
    Put32(args, static_cast<uint32_t>(24U + node.size()));
    args.insert(args.end(), 16U, 0U);
    Put32(args, static_cast<uint32_t>(node.size()));
    args.insert(args.end(), node.begin(), node.end());
    return args;
}

const char *Status(uint32_t status)
{
    switch (static_cast<UrmaProbeStatus>(status)) {
        case UrmaProbeStatus::kSymbolsAvailable: return "symbols_available";
        case UrmaProbeStatus::kLoaderUnavailable: return "loader_unavailable";
        case UrmaProbeStatus::kLibraryUnavailable: return "library_unavailable";
        case UrmaProbeStatus::kMissingSymbols: return "missing_symbols";
        case UrmaProbeStatus::kCloseFailed: return "close_failed";
    }
    return "invalid";
}

int Report(const UrmaProbeBlock &request, const UrmaProbeBlock &report)
{
    const bool valid = std::memcmp(&request, &report, offsetof(UrmaProbeBlock, status)) == 0 &&
        report.done == 1U && report.echoedCookie == request.cookie &&
        report.symbolCount == kUrmaProbeSymbolCount && (report.presentMask & ~kUrmaProbeAllSymbols) == 0U &&
        report.status <= static_cast<uint32_t>(UrmaProbeStatus::kCloseFailed) &&
        report.source <= static_cast<uint32_t>(UrmaProbeSource::kOpenedLibrary) &&
        std::memchr(report.library, '\0', sizeof(report.library)) != nullptr &&
        std::memchr(report.error, '\0', sizeof(report.error)) != nullptr;
    if (!valid) {
        std::puts("report=INVALID hint=verify_CI_package_contains_UMP1_probe");
        return 1;
    }
    const char *source = report.source == 1U ? "process" : report.source == 2U ? "opened_library" : "none";
    std::printf("report=VALID status=%s architecture=%s source=%s\n", Status(report.status),
                report.architecture == 1U ? "AArch64" : report.architecture == 2U ? "x86_64" : "other", source);
    std::printf("symbol_owner=%s\n", report.library[0] != '\0' ? report.library : "<unavailable>");
    unsigned present = 0U;
    for (size_t i = 0; i < kUrmaProbeSymbolCount; ++i) {
        if ((report.presentMask & (UINT64_C(1) << i)) != 0U) { ++present; }
    }
    std::printf("symbols_present=%u symbols_checked=%zu\n", present, kUrmaProbeSymbolCount);
    if (present != kUrmaProbeSymbolCount) {
        std::printf("missing_symbols=");
        const char *separator = "";
        for (size_t i = 0; i < kUrmaProbeSymbolCount; ++i) {
            if ((report.presentMask & (UINT64_C(1) << i)) == 0U) {
                std::printf("%s%s", separator, kUrmaProbeSymbols[i]);
                separator = ",";
            }
        }
        std::putchar('\n');
    }
    // Bound/sanitize a loader error so unusual embedded newlines do not inflate the report.
    if (report.error[0] != '\0') {
        std::printf("loader_detail=");
        for (const char *p = report.error; *p != '\0'; ++p) {
            std::putchar(static_cast<unsigned char>(*p) >= 32U && *p != 127 ? *p : ' ');
        }
        std::putchar('\n');
    }
    if (report.architecture != 1U) { return 1; }
    if (report.status == static_cast<uint32_t>(UrmaProbeStatus::kCloseFailed)) { return 1; }
    if (report.status != 0U) { return 3; }
    return report.presentMask == kUrmaProbeAllSymbols && report.source != 0U ? 0 : 1;
}

const char *ResourceStatus(uint32_t value)
{
    switch (static_cast<UrmaResourceStatus>(value)) {
        case UrmaResourceStatus::kOk: return "ok";
        case UrmaResourceStatus::kMissingSymbols: return "missing_symbols";
        case UrmaResourceStatus::kNoDevices: return "no_devices_or_eids";
        case UrmaResourceStatus::kEnumerationFailed: return "enumeration_failed";
        case UrmaResourceStatus::kSelectionNotFound: return "selection_not_found";
        case UrmaResourceStatus::kApiFailed: return "api_failed";
        case UrmaResourceStatus::kCleanupFailed: return "cleanup_failed";
        case UrmaResourceStatus::kMalformedApiResult: return "malformed_api_result";
        case UrmaResourceStatus::kUnexpectedCompletion: return "unexpected_completion";
    }
    return "invalid";
}

bool EndpointValid(const UrmaEndpoint &endpoint)
{
    for (std::size_t i = 0; i < sizeof(endpoint.name); ++i) {
        const char c = endpoint.name[i];
        if (c == '\0') { return i != 0U && endpoint.reserved == 0U; }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) { return false; }
    }
    return false;
}

void PrintEndpoint(const char *label, const UrmaEndpoint &endpoint)
{
    std::printf("%s=%s transport=%d eid_index=%u eid=", label, endpoint.name, endpoint.transport, endpoint.eidIndex);
    for (auto byte : endpoint.eid) { std::printf("%02x", byte); }
    std::putchar('\n');
}

int ResourceReport(const UrmaResourceBlock &request, const UrmaResourceBlock &report)
{
    bool valid = std::memcmp(&request, &report, offsetof(UrmaResourceBlock, status)) == 0 &&
        report.done == 1U && report.echoedCookie == request.cookie && report.initPolicy == 1U &&
        report.status <= static_cast<uint32_t>(UrmaResourceStatus::kUnexpectedCompletion) &&
        report.reportedCount <= kUrmaEndpointLimit && report.reportedCount <= report.endpointCount &&
        report.endpointCount <= 64U * 1024U && report.deviceCount <= 64U && report.cleanupRetained <= 1U &&
        (report.presentMask & ~UINT64_C(0x1fff)) == 0U &&
        std::memchr(report.library, '\0', sizeof(report.library)) != nullptr &&
        std::memchr(report.detail, '\0', sizeof(report.detail)) != nullptr;
    for (const auto &step : report.steps) { valid = valid && step.state <= 2U && step.reserved == 0U; }
    for (uint32_t i = 0; valid && i < report.reportedCount; ++i) {
        valid = EndpointValid(report.endpoints[i]) && report.endpoints[i].deviceOrdinal < report.deviceCount;
    }
    const bool selected = report.steps[static_cast<unsigned>(UrmaResourceStep::kSelect)].state == 1U;
    if (selected) {
        valid = valid && EndpointValid(report.selected) && report.selected.deviceOrdinal < report.deviceCount &&
            std::strcmp(report.selected.name, request.deviceName) == 0 && report.selected.eidIndex == request.eidIndex &&
            report.selected.transport == urma_probe_abi::kTransportUb;
    }
    if (!valid) { std::puts("report=INVALID hint=verify_CI_package_contains_UMP2_probe"); return 1; }
    std::printf("report=VALID status=%s architecture=%s\n", ResourceStatus(report.status),
                report.architecture == 1U ? "AArch64" : report.architecture == 2U ? "x86_64" : "other");
    std::puts("initialization=existing_process init_calls=0 uninit_calls=0");
    std::printf("symbol_owner=%s\n", report.library[0] != '\0' ? report.library : "<unavailable>");
    std::printf("device_count=%u enumerated_eids=%u reported_eids=%u omitted_eids=%u\n", report.deviceCount,
                report.endpointCount, report.reportedCount, report.endpointCount - report.reportedCount);
    if (request.mode == static_cast<uint32_t>(UrmaResourceMode::kList)) {
        for (uint32_t i = 0; i < report.reportedCount; ++i) { PrintEndpoint("urma_device", report.endpoints[i]); }
    } else if (selected) {
        PrintEndpoint("selected_urma_device", report.selected);
        std::printf("requested_queue_depth=%u transport=RM max_sge=1 max_remote_sge=1\n", request.queueDepth);
    }
    for (std::size_t i = 0; i < kUrmaResourceStepCount; ++i) {
        const auto &step = report.steps[i];
        if (step.state != 0U) {
            std::printf("device_step=%s %s rc=%d errno=%d\n", kUrmaResourceStepNames[i],
                        step.state == 1U ? "OK" : "FAIL", step.result, step.systemError);
        }
    }
    if (report.detail[0] != '\0') { std::printf("device_detail=%.127s\n", report.detail); }
    if (report.cleanupRetained != 0U) { std::puts("device_cleanup=INCOMPLETE surviving_resources_retained=1"); }
    if (report.architecture != 1U || report.cleanupRetained != 0U) { return 1; }
    if (report.status == static_cast<uint32_t>(UrmaResourceStatus::kMissingSymbols) ||
        report.status == static_cast<uint32_t>(UrmaResourceStatus::kNoDevices)) { return 3; }
    if (report.status != 0U) { return 1; }
    // A success status without evidence for every required step is not a PASS.
    const bool resources = request.mode == static_cast<uint32_t>(UrmaResourceMode::kResources);
    const std::size_t required = resources ? kUrmaResourceStepCount : 3U;
    if (report.presentMask != UINT64_C(0x1fff) || report.endpointCount == 0U || report.reportedCount == 0U) { return 1; }
    for (std::size_t i = 0; i < required; ++i) {
        if (report.steps[i].state != 1U || (i != 1U && report.steps[i].result != 0)) { return 1; }
    }
    if (!resources) {
        for (std::size_t i = required; i < kUrmaResourceStepCount; ++i) {
            if (report.steps[i].state != 0U) { return 1; }
        }
    }
    return 0;
}

void PrintResult(const char *key, int result)
{
    const char *queues = std::strcmp(key, "aicpu_urma_resources") == 0 ? "" : " device_queue_creation=NOT_TESTED";
    std::printf("RESULT %s=%s%s device_send=NOT_TESTED graph_replay=NOT_TESTED\n", key,
                result == 0 ? "PASS" : result == 3 ? "UNAVAILABLE" : "FAIL", queues);
}

template <typename Block>
int Run(int32_t device, int32_t timeout, bool allowLoad, Block &request,
        int (*interpret)(const Block &, const Block &), const char *probe, const char *resultKey)
{
    std::printf("probe=%s version=%u device=%d allow_load=%u timeout_ms=%d\n",
                probe, request.version, device, allowLoad ? 1U : 0U, timeout);
    Dl_info owner{};
    void *symbol = dlsym(RTLD_NEXT, "rtSetDevice");
    std::printf("loaded_runtime=%s\n", symbol != nullptr && dladdr(symbol, &owner) != 0 &&
                owner.dli_fname != nullptr ? owner.dli_fname : "<unresolved>");
    const char *package = std::getenv("ASCEND_AICPU_PATH");
    std::printf("ASCEND_AICPU_PATH=%s dispatch=platform_by_name\n", package != nullptr ? package : "<unset>");
    if (!Step("set_device", rtSetDevice(device))) { return 1; }
    rtStream_t stream = nullptr;
    if (!Step("create_stream", rtStreamCreate(&stream, 0))) { return 1; }
    void *deviceBlock = nullptr;
    int result = 1;
    bool completed = true;
    request.bytes = sizeof(request);
    request.cookie = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1U;
    // A stale copy of the request must never look like a successful report.
    request.status = UINT32_MAX;
    std::vector<uint8_t> params;
    do {
        if (!Step("allocate_report", rtMalloc(&deviceBlock, sizeof(request), RT_MEMORY_HBM, 0))) { break; }
        if (reinterpret_cast<uintptr_t>(deviceBlock) % alignof(Block) != 0U) {
            std::puts("report_allocation=FAIL reason=unaligned");
            break;
        }
        if (!Step("write_request", rtMemcpy(deviceBlock, sizeof(request), &request, sizeof(request),
                                             RT_MEMCPY_HOST_TO_DEVICE))) { break; }
        params = Arguments(reinterpret_cast<uint64_t>(deviceBlock));
        rtArgsEx_t args{};
        args.args = params.data();
        args.argsSize = static_cast<uint32_t>(params.size());
        const rtKernelLaunchNames_t names{"libaicpu_kernels.so", "SecureDma", "SecureDma"};
        // Even a launch error may need draining. Release request memory only after successful native sync.
        completed = false;
        const bool launched = Step("launch", rtAicpuKernelLaunchWithFlag(
            &names, 1U, &args, nullptr, stream, RT_KERNEL_DEFAULT));
        completed = Step("synchronize", rtStreamSynchronizeWithTimeout(stream, timeout));
        if (!launched || !completed) {
            std::puts("hint=check_device_log_and_CI_package_for_requested_probe_version");
            break;
        }
        Block report{};
        if (!Step("read_report", rtMemcpy(&report, sizeof(report), deviceBlock, sizeof(report),
                                           RT_MEMCPY_DEVICE_TO_HOST))) { break; }
        result = interpret(request, report);
    } while (false);
    if (!completed) {
        std::puts("cleanup=DEFERRED_TO_PROCESS_EXIT reason=native_completion_unconfirmed");
        // Do not free memory or destroy a stream that could still be used by the device.
        // Skip Runtime's process destructors too: their drain may be unbounded after a timeout.
        PrintResult(resultKey, 1);
        std::fflush(nullptr);
        std::_Exit(1);
    }
    if (deviceBlock != nullptr && !Step("free_report", rtFree(deviceBlock))) { result = 1; }
    if (!Step("destroy_stream", rtStreamDestroy(stream))) { result = 1; }
    return result;
}
} // namespace

int main(int argc, char **argv)
{
    int32_t device = 0;
    int32_t timeout = 10000;
    bool allowLoad = false;
    uint32_t mode = 0U; // 0 = the backward-compatible symbol probe
    const char *urmaDevice = nullptr;
    int32_t eidIndex = -1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allow-load") == 0) { allowLoad = true; }
        else if (std::strcmp(argv[i], "--list") == 0 || std::strcmp(argv[i], "--resources") == 0) {
            const uint32_t wanted = std::strcmp(argv[i], "--list") == 0 ? 1U : 2U;
            if (mode != 0U && mode != wanted) { std::fputs("Choose only one of --list and --resources.\n", stderr); return 2; }
            mode = wanted;
        }
        else if (std::strcmp(argv[i], "--urma-device") == 0) {
            if (i + 1 >= argc) { std::fputs("--urma-device requires a name from the AICPU list.\n", stderr); return 2; }
            urmaDevice = argv[++i];
        }
        else if (std::strcmp(argv[i], "--device") == 0 || std::strcmp(argv[i], "--timeout-ms") == 0 ||
                 std::strcmp(argv[i], "--eid-index") == 0) {
            const bool isDevice = std::strcmp(argv[i], "--device") == 0;
            const bool isEid = std::strcmp(argv[i], "--eid-index") == 0;
            if (i + 1 >= argc || !Number(argv[i + 1], isDevice || isEid ? 0 : 1,
                                        isDevice || isEid ? INT32_MAX : 30000, isDevice ? device : isEid ? eidIndex : timeout)) {
                std::fprintf(stderr, "Invalid or missing value for %s\n", argv[i]);
                return 2;
            }
            ++i;
        }
        else {
            std::fprintf(stderr, "Usage: %s [--device 0] [--timeout-ms 10000] [--allow-load]\n"
                "       %s --device 0 --list\n"
                "       %s --device 0 --resources --urma-device NAME --eid-index INDEX\n", argv[0], argv[0], argv[0]);
            return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *sideLoad = std::getenv("SECURE_AICPU_SO_PATH");
    if (sideLoad != nullptr && *sideLoad != '\0') {
        std::fputs("This launcher uses platform dispatch; SECURE_AICPU_SO_PATH must be unset.\n", stderr);
        return 2;
    }
    if ((mode != 0U && allowLoad) || (mode != 2U && (urmaDevice != nullptr || eidIndex >= 0)) ||
        (mode == 2U && (urmaDevice == nullptr || eidIndex < 0))) {
        std::fputs("Resource mode needs --urma-device and --eid-index; --allow-load is only for symbol mode.\n", stderr);
        return 2;
    }
    int result;
    const char *resultKey;
    if (mode == 0U) {
        UrmaProbeBlock request{};
        request.magic = kUrmaProbeMagic;
        request.version = kUrmaProbeVersion;
        request.flags = allowLoad ? kUrmaProbeAllowLoad : 0U;
        resultKey = "aicpu_urma_symbols";
        result = Run(device, timeout, allowLoad, request, Report, resultKey, resultKey);
    } else {
#ifndef SECURE_DMA_PROBE_HAVE_URMA_HEADERS
        std::fputs("Resource modes require rebuilding with the target's installed URMA headers (URMA_INCLUDE_DIR).\n", stderr);
        return 2;
#endif
        UrmaResourceBlock request{};
        request.magic = kUrmaResourceMagic;
        request.version = kUrmaResourceVersion;
        request.mode = mode;
        request.abiTag = urma_probe_abi::kLayoutTag;
        request.queueDepth = kUrmaResourceDepth;
        if (mode == 2U) {
            if (std::strlen(urmaDevice) >= sizeof(request.deviceName)) {
                std::fputs("URMA device name must be 1..63 characters.\n", stderr); return 2;
            }
            std::strcpy(request.deviceName, urmaDevice);
            UrmaEndpoint checked{};
            std::strcpy(checked.name, urmaDevice);
            if (!EndpointValid(checked)) { std::fputs("Invalid URMA device name.\n", stderr); return 2; }
            request.eidIndex = static_cast<uint32_t>(eidIndex);
        }
        std::puts("abi_check=HOST_HEADERS_MATCH device_library_abi=UNCONFIRMED");
        resultKey = mode == 1U ? "aicpu_urma_devices" : "aicpu_urma_resources";
        result = Run(device, timeout, false, request, ResourceReport, resultKey, resultKey);
    }
    PrintResult(resultKey, result);
    return result;
}
