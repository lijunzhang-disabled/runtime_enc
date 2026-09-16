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

int Run(int32_t device, int32_t timeout, bool allowLoad)
{
    std::printf("probe=aicpu_urma_symbols version=1 device=%d allow_load=%u timeout_ms=%d\n",
                device, allowLoad ? 1U : 0U, timeout);
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
    UrmaProbeBlock request{};
    request.magic = kUrmaProbeMagic;
    request.version = kUrmaProbeVersion;
    request.bytes = sizeof(request);
    request.flags = allowLoad ? kUrmaProbeAllowLoad : 0U;
    request.cookie = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1U;
    // A stale copy of the request must never look like a successful report.
    request.status = UINT32_MAX;
    std::vector<uint8_t> params;
    do {
        if (!Step("allocate_report", rtMalloc(&deviceBlock, sizeof(request), RT_MEMORY_HBM, 0))) { break; }
        if (reinterpret_cast<uintptr_t>(deviceBlock) % alignof(UrmaProbeBlock) != 0U) {
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
            std::puts("hint=check_device_log_and_CI_package_for_UMP1_support");
            break;
        }
        UrmaProbeBlock report{};
        if (!Step("read_report", rtMemcpy(&report, sizeof(report), deviceBlock, sizeof(report),
                                           RT_MEMCPY_DEVICE_TO_HOST))) { break; }
        result = Report(request, report);
    } while (false);
    if (!completed) {
        std::puts("cleanup=DEFERRED_TO_PROCESS_EXIT reason=native_completion_unconfirmed");
        // Do not free memory or destroy a stream that could still be used by the device.
        // Skip Runtime's process destructors too: their drain may be unbounded after a timeout.
        std::puts("RESULT aicpu_urma_symbols=FAIL device_queue_creation=NOT_TESTED device_send=NOT_TESTED graph_replay=NOT_TESTED");
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
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--allow-load") == 0) { allowLoad = true; }
        else if (std::strcmp(argv[i], "--device") == 0 || std::strcmp(argv[i], "--timeout-ms") == 0) {
            const bool isDevice = std::strcmp(argv[i], "--device") == 0;
            if (i + 1 >= argc || !Number(argv[i + 1], isDevice ? 0 : 1,
                                        isDevice ? INT32_MAX : 30000, isDevice ? device : timeout)) {
                std::fprintf(stderr, "Invalid or missing value for %s\n", argv[i]);
                return 2;
            }
            ++i;
        }
        else {
            std::fprintf(stderr, "Usage: %s [--device 0] [--timeout-ms 10000] [--allow-load]\n", argv[0]);
            return std::strcmp(argv[i], "--help") == 0 ? 0 : 2;
        }
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *sideLoad = std::getenv("SECURE_AICPU_SO_PATH");
    if (sideLoad != nullptr && *sideLoad != '\0') {
        std::fputs("This launcher uses platform dispatch; SECURE_AICPU_SO_PATH must be unset.\n", stderr);
        return 2;
    }
    const int result = Run(device, timeout, allowLoad);
    std::printf("RESULT aicpu_urma_symbols=%s device_queue_creation=NOT_TESTED device_send=NOT_TESTED graph_replay=NOT_TESTED\n",
                result == 0 ? "PASS" : result == 3 ? "UNAVAILABLE" : "FAIL");
    return result;
}
