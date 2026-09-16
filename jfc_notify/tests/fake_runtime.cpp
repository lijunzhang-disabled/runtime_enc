// Host launcher tests only: no hardware or vendor library is used.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "runtime/dev.h"
#include "runtime/kernel.h"
#include "runtime/mem.h"
#include "runtime/stream.h"
#include "secure_dma_urma_probe_protocol.h"

using namespace secure_dma;
namespace {
alignas(64) UrmaProbeBlock block;
bool launched = false, synchronized = false;
int streamToken;
bool Is(const char *name)
{
    const char *value = std::getenv("MOCK_CASE");
    return value != nullptr && std::strcmp(value, name) == 0;
}
uint32_t Word(const uint8_t *p)
{
    uint32_t value;
    std::memcpy(&value, p, 4U);
    return value;
}
}

extern "C" rtError_t rtSetDevice(int32_t device)
{
    assert(device == 0);
    return Is("set_fail") ? 1 : RT_ERROR_NONE;
}
extern "C" rtError_t rtStreamCreate(rtStream_t *stream, int32_t priority)
{
    assert(priority == 0);
    if (Is("stream_fail")) { return 1; }
    *stream = &streamToken;
    return RT_ERROR_NONE;
}
extern "C" rtError_t rtMalloc(void **ptr, uint64_t size, rtMemType_t type, uint16_t module)
{
    assert(size == sizeof(block) && type == RT_MEMORY_HBM && module == 0);
    if (Is("malloc_fail")) { return 1; }
    *ptr = &block;
    return RT_ERROR_NONE;
}
extern "C" rtError_t rtMemcpy(void *dst, uint64_t capacity, const void *src, uint64_t size, rtMemcpyKind_t kind)
{
    assert(size == sizeof(block) && capacity == size);
    if (kind == RT_MEMCPY_HOST_TO_DEVICE) {
        assert(!launched && dst == &block);
        if (Is("write_fail")) { return 1; }
    } else {
        assert(kind == RT_MEMCPY_DEVICE_TO_HOST && src == &block && launched && synchronized);
        if (Is("read_fail")) { return 1; }
    }
    std::memcpy(dst, src, size);
    return RT_ERROR_NONE;
}
extern "C" rtError_t rtAicpuKernelLaunchWithFlag(const rtKernelLaunchNames_t *names, uint32_t blocks,
    const rtArgsEx_t *args, rtSmDesc_t *desc, rtStream_t stream, uint32_t flags)
{
    assert(std::strcmp(names->soName, "libaicpu_kernels.so") == 0);
    assert(std::strcmp(names->kernelName, "SecureDma") == 0 && std::strcmp(names->opName, "SecureDma") == 0);
    assert(blocks == 1U && desc == nullptr && stream == &streamToken && flags == RT_KERNEL_DEFAULT);
    rtArgsEx_t expected{};
    expected.args = args->args; expected.argsSize = args->argsSize;
    assert(std::memcmp(args, &expected, sizeof(expected)) == 0);
    // Independent byte contract for the 20-byte framework header and protobuf fields.
    const auto *p = static_cast<const uint8_t *>(args->args);
    const uint8_t prefix[] = {0x12, 9, 'S','e','c','u','r','e','D','m','a', 0x1a, 34,
        0x0a, 12, 's','c','c','_','d','m','a','_','a','r','g','s', 0x12, 18, 0x12, 16};
    assert(args->argsSize == 71U && Word(p) == 71U && Word(p + 20U) == 47U);
    for (unsigned i = 4; i < 20; ++i) { assert(p[i] == 0U); }
    assert(std::memcmp(p + 24U, prefix, sizeof(prefix)) == 0);
    uint64_t anchor = 0U, trace = 1U;
    std::memcpy(&anchor, p + 24U + sizeof(prefix), 8U);
    std::memcpy(&trace, p + 32U + sizeof(prefix), 8U);
    assert(anchor == reinterpret_cast<uint64_t>(&block) && trace == 0U);
    assert(block.magic == kUrmaProbeMagic && block.version == 1U && block.bytes == 640U && block.cookie != 0U);
    assert(block.flags == (Is("allow_load") ? 1U : 0U));
    launched = true;
    return Is("launch_fail") ? 1 : RT_ERROR_NONE;
}
extern "C" rtError_t rtStreamSynchronizeWithTimeout(rtStream_t stream, int32_t timeout)
{
    assert(stream == &streamToken && launched && timeout > 0 && timeout <= 30000);
    if (Is("sync_fail")) { return 1; }
    synchronized = true;
    // Synthetic AArch64 report for validating Host interpretation, never a hardware result.
    block.architecture = 1U; block.status = 0U; block.source = 1U;
    block.symbolCount = kUrmaProbeSymbolCount; block.presentMask = kUrmaProbeAllSymbols;
    block.echoedCookie = block.cookie; block.done = 1U;
    std::strcpy(block.library, "/mock/aicpu/liburma.so");
    if (Is("unavailable")) { block.status = 2U; block.source = 0U; block.presentMask = 0U; }
    if (Is("partial")) { block.status = 3U; block.presentMask &= ~(UINT64_C(1) << 22U); }
    if (Is("close_fail")) { block.status = 4U; }
    if (Is("bad_cookie")) { ++block.echoedCookie; }
    if (Is("bad_magic")) { ++block.magic; }
    if (Is("bad_done")) { block.done = 0U; }
    if (Is("bad_count")) { ++block.symbolCount; }
    if (Is("bad_mask")) { block.presentMask |= UINT64_C(1) << 63U; }
    if (Is("bad_arch")) { block.architecture = 2U; }
    if (Is("bad_status")) { block.status = UINT32_MAX; }
    if (Is("bad_source")) { block.source = UINT32_MAX; }
    if (Is("bad_text")) { std::memset(block.error, 'X', sizeof(block.error)); }
    return RT_ERROR_NONE;
}
extern "C" rtError_t rtFree(void *ptr)
{
    assert(ptr == &block && (!launched || synchronized));
    std::puts("mock=free");
    return Is("free_fail") ? 1 : RT_ERROR_NONE;
}
extern "C" rtError_t rtStreamDestroy(rtStream_t stream)
{
    assert(stream == &streamToken && (!launched || synchronized));
    std::puts("mock=destroy");
    return Is("destroy_fail") ? 1 : RT_ERROR_NONE;
}
