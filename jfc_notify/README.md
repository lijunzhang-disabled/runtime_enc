# JFC notification experiment: first step

**Current step:** Host receive resources passed on the target. The AICPU URMA symbol probe is now
prepared on local AICPU branch `iter-007-zlj`, based on `iter-007`. Build those changes through the
existing online CI, deploy the resulting platform package, then use the [NPU 0 instructions below](#aicpu-symbol-probe-ci-and-target-run).
Target execution of this new probe is pending.

Start with a Host inventory on the actual UB machine. This identifies the installed driver, URMA library/provider candidates, headers, and exported entry points needed to build the first functional probe.

The eventual first functional test is: prepost a Host JFR receive buffer, SEND a changing sequence value from the intended device execution context, and receive a matching successful Host JFC completion. There is no crypto or graph integration in that first test.

## Run the inventory

Copy `collect_target_inventory.py` to the target. From the directory containing it, run:

```bash
python3 collect_target_inventory.py --output target-urma-inventory.json
```

The default report is concise: driver/compiler/device summaries, up to three canonical library candidates per kind, up to three paths per header name, and missing URMA exports. Duplicate aliases and raw command output are excluded. Counts show how many candidates were omitted; listed paths are not a claim about which library is loaded.

If you already generated a large report, convert it with the updated script without running inventory commands again:

```bash
python3 collect_target_inventory.py \
  --from-report target-urma-inventory.json \
  --output target-urma-summary.json
```

Use `--full` only if the full inventory is needed for follow-up diagnosis.

Use the same shell/library environment normally used to run your isolated `iter-007` Runtime. If the SDK or isolated installation is outside the standard directories, add its root:

```bash
python3 collect_target_inventory.py \
  --search-root /path/to/installed/sdk \
  --search-root /path/to/isolated/runtime \
  --output target-urma-inventory-with-sdk.json
```

Python 3 is required. `readelf`, `npu-smi`, and a C++ compiler are inspected when available; missing tools are recorded. No root access is requested. Existing output files are not overwritten.

The script performs filesystem/ELF inspection and runs bounded inventory commands (`npu-smi info`, `ldconfig -p`, `readelf`, and compiler version). It does not load vendor libraries, create contexts/queues, submit work, install packages, or reset devices. Its only writes are the requested report and its parent directories.

Share the concise JSON report. It contains installation paths and device inventory; it does not collect key material or the complete process environment.

## How to interpret the result

- Exit code 0 means the inventory was written, not that a notification test passed.
- Exported URMA symbols establish candidate API availability only. They do not prove that the provider supports a particular queue or that the AICPU process can use it.
- Multiple library versions must be resolved before compilation. The script lists candidates, not the library a future application will necessarily load.
- An absent header or library is a search result, not proof that the target lacks support. Add a narrower `--search-root` for a nonstandard install. Directory symlinks are not recursively followed, and searches have depth/entry limits.
- The script runs on the Host. Device/AICPU loader access, context creation, memory visibility, and notification delivery remain untested.

## Target inventory received on 2026-09-15

The supplied report identifies an AArch64 Host with kernel `6.6.0-ub+`, Ascend950DT devices, driver `25.6.rc1.b163`, HAL `7.35.23`, and GCC `12.3.1`. It found:

- URMA core: `/usr/lib64/liburma.so.0.0.3`; all 26 inspected exports are present.
- UDMA provider candidate: `/usr/lib64/urma/liburma-udma.so`.
- Matching installed header candidates: `/usr/include/ub/umdk/urma/`.
- Ten entries in `/sys/class/uburma`; the names have the form `udmac0d1e2`.

This was enough to build the Host resource probe. The library filename alone does not establish a package release or header/library ABI match. The directory permission errors make the filesystem inventory incomplete, but the required Host header/core/provider candidates were found. No `libaicpu_kernels.so` candidate appeared in this search; that does not establish its absence from the device environment. Subsequent functional results are recorded below.

The subsequent target enumeration **passed**: URMA initialization, device/EID enumeration, and uninitialization succeeded. It returned ten devices, `udmac{0,1}d1e{2,3,4,5,6}`. All have EID index 0; each `e6` device has nine EIDs. Version 1 reported the executable as `loaded_urma`; this diagnostic bug is reproducible locally with a non-PIE build, where the function address identifies a PLT entry. Version 2 resolves the symbol definition through `dlsym(RTLD_NEXT, ...)` before inspecting its owner.

## Host receive-resource result: PASS

The user ran version 2 on the target and supplied the [complete short result](host_receive_result_2026-09-15.txt). This is target evidence, distinct from the local mock tests.

| Check | Observed result |
| --- | --- |
| Loaded URMA core | `/usr/lib64/liburma.so.0.0.3` |
| Selected endpoint | `udmac0d1e2`, EID index `0` |
| Reported transport modes | `0x7`: RM, RC, and UM capability bits |
| Reported limits | JFC depth 1,048,576; JFR depth 32,768; 4 receive SGEs |
| Actual resources tested | One context, one JFCE, depth-16 JFC/JFR |
| Ordinary Host memory registration | Passed; one page registered with local-only segment access |
| Receive posting | One 64-byte slot accepted |
| Empty JFC poll | Returned 0, as expected without a sender |
| Resource cleanup | Every teardown call succeeded |

The reported limits are capabilities, not measured capacities or performance. Only the small resource configuration above was exercised. Receiving a completion, event wakeup, device access, endpoint routing to the selected NPU, and graph replay remain untested.

## Build and run the Host receive-resource probe

Copy `host_receive_probe.c` and `build_host_probe.sh` to this directory on the target. Build in the same environment as the inventory:

```bash
bash build_host_probe.sh
./host_receive_probe --list > host-urma-list.txt 2>&1
cat host-urma-list.txt
```

The build uses the installed target headers and the reported versioned library path, so it does not require an unversioned `liburma.so` development symlink. Override `URMA_INCLUDE_DIR`, `URMA_LIBRARY`, `CC`, or `PROBE_OUTPUT` if needed. The binary prints `loaded_urma` because the dynamic loader can choose a different library via its normal search rules. Compare that path with the build's link library.

List mode initializes URMA, lists device names and valid EID indices, and uninitializes it. It creates no contexts or queues. At most eight EIDs per device are printed, with an omitted count. Share this short text first; a UB device name or EID index is not an NPU logical ID, and this probe does not establish their mapping.

After selecting a listed endpoint, run the resource check with its exact name and index:

```bash
./host_receive_probe --device DEVICE_NAME --eid-index EID_INDEX \
  > host-urma-resources.txt 2>&1
cat host-urma-resources.txt
```

Replace `DEVICE_NAME` and `EID_INDEX` with values from the list. The probe validates the selection and performs these steps:

1. Query the device's RM transport, queue-depth, and receive-SGE capabilities.
2. Create one context, one JFCE, one depth-16 JFC, and one depth-16 JFR.
3. Allocate and register one page of ordinary Host memory with local-only segment access; use a fresh token for this process's resources.
4. Post one 64-byte receive slot and call `urma_poll_jfc()` once. Zero completions is expected because this stage has no sender.
5. Delete the JFR before unregistering/freeing its posted buffer, then delete the JFC, JFCE, and context and uninitialize URMA.

Each step prints one short status line. `host_receive_resources=PASS` requires all steps and cleanup to succeed. A cleanup error stops dependent releases and returns failure. A registration failure describes this ordinary pinned-memory setup; it does not rule out another provider-supported memory allocation path. The probe never imports or exports a remote endpoint, submits SEND/READ/WRITE work, or arms/waits for events.

For the target enumeration already received, a concrete first resource check is:

```bash
bash build_host_probe.sh
./host_receive_probe --device udmac0d1e2 --eid-index 0 \
  > host-urma-resources.txt 2>&1
cat host-urma-resources.txt
```

Use the updated version-2 source when rebuilding. This pair is explicitly present in the report, with EID `00000000003f020000100000df080b00`. It is a Host resource-test candidate, not a confirmed route to a particular NPU. Before device SEND, establish the intended NPU's endpoint mapping; the inspected driver's UB connection path obtains that mapping through `dms_get_ub_dev_info`, rather than deriving it from the `udmac...` name.

This checks Host receive-resource setup and an empty poll only. It does **not** prove receipt of a message, event wakeup, device visibility, or graph replay. The next functional probe must add a sender in the actual intended AICPU execution context and validate both its local send completion and the Host's receive completion.

Local validation uses the source-reference headers and a mock URMA library. It checks compilation, command-line validation, call order, and failure cleanup. The target run above additionally confirms successful resource creation, registration, posting, empty polling, and cleanup on the selected endpoint; failure cases have not been exercised on hardware. Queue configuration follows the driver's HDC JFR example and HCOMM's local-only segment registration, without copying their application-specific memory allocator or remote endpoint setup.

## Next two functional checks

1. **Host receive resources — passed on the selected endpoint:** compiled against target headers/library; enumerated device/EID choices; created a dedicated JFCE/JFC/JFR, registered and posted a receive buffer, and cleanly tore down the resources.
2. **Device SEND:** establish the matching device endpoint in the actual intended execution context, send one synthetic message, validate the Host receive CQE/message, consume the device's local send completion, and release resources safely. Then repeat enough messages to exercise receive replenishment.

Only after these checks pass should we add capture/replay and then secure-copy integration. A SEND from a generic device process does not establish that the SecureDma AICPU execution process can submit it.

### Preparing the device SEND check

The user selected **NPU logical ID 0** for the next probe. Its UB endpoint mapping is still unverified. The user confirmed that AICPU kernels are built by their online CI platform. Device-source changes belong in `AscendCCv2-AICPU/ms_kernels/`, with the source list and compile definitions in `ms_kernels/secure_dma_v2.cmake`; the platform build integrates these into `libaicpu_kernels.so` and packages the result. The local AICPU repository's top-level CMake exposes only the device-source contract and creates no build targets. Source preparation and local checks can proceed using this integration contract; the user will build the resulting device artifact through the existing CI flow and run it on the target. Runtime's `Dispatch` supports both platform dispatch by name and `SECURE_AICPU_SO_PATH` side loading, with built-in AICPU scheduler selection in both cases. Use the platform-dispatch mode already reported by the user.

To locate the current artifact, first inspect `SECURE_AICPU_SO_PATH` in the workload's launch environment. If unset, Runtime dispatches `SecureDma` by name; the Host installation may contain an AICPU archive instead of an unpacked `.so`. The inspected TSD package loader uses `ASCEND_AICPU_PATH` (default `/usr/local/Ascend/`) and an `opp/<package-title>/aicpu/` directory containing an `*aicpu_syskernels.tar.gz` package. Device-side extraction can use `/usr/lib64/aicpu_kernels/<uniqueVfId>/aicpu_kernels_device/` or another run-mode-specific path; that path need not be visible on the Host, and `uniqueVfId` must not be assumed to equal the NPU logical ID. These source-derived locations are search candidates until confirmed on the target.

The user found these Host package candidates:

- `/home/zlj/cann/cann/opp/Ascend/aicpu/Ascend-aicpu_syskernels.tar.gz`
- `/home/zlj/cann/cann/opp/Ascend/aicpu/Ascend-aicpu_extend_syskernels.tar.gz`

On 2026-09-16 the user confirmed `SECURE_AICPU_SO_PATH` is unset and `ASCEND_AICPU_PATH=/home/zlj/cann/cann-9.1.T560`. Runtime therefore uses platform dispatch by name in that environment. Whether the earlier `/home/zlj/cann/cann` path resolves to this same installation remains unconfirmed. Running `tar -tzf` on the earlier package failed with `not in gzip format`; its suffix alone does not identify the outer file format.

The inspected Runtime TSD reader handles a CMS package with an 8192-byte header and a 256-byte descriptor before the archive payload. The user's subsequent inspection confirmed a gzip payload at byte 8448 in both selected packages, matching this source-reference offset; signature verification was not performed. See `PackageVerify::ProcessSendStepVerify` in `/home/zlj/ascend_stack/runtime/src/tsd/basic_component/package_manager/src/package_verify.cpp`.

Copy `inspect_aicpu_package.py` to the target and run it in the workload's launch environment:

```bash
python3 inspect_aicpu_package.py
```

It reads both packages under `$ASCEND_AICPU_PATH/opp/Ascend/aicpu/`, prints their resolved paths, and checks for an ordinary archive or an archive at the source-reference payload offset. It reports at most four matching kernel-library members per package, ELF architecture, whether `SecureDma` is a defined exported symbol, a short dependency list, and version-file summaries. It uses Python's standard library and `readelf`; no `rg` is required. To inspect another candidate, use `--package /path/to/package` (repeatable).

The inspector opens installed packages read-only and copies selected regular members into temporary files for `readelf`; it never loads a library, changes an installed package, or follows archived symlinks. It does not authenticate a signature. An unknown format is reported with short byte prefixes; it is not guessed or rewritten. Library and total archive inspection have size limits, with incomplete inspection reported explicitly. Output is static package evidence, not proof that a particular device process loaded the binary or can use URMA.

### Package inspection result: SecureDma export confirmed

The user supplied the [complete package inspection result](aicpu_package_result_2026-09-16.txt):

| Item | Observed result |
| --- | --- |
| Main package | `$ASCEND_AICPU_PATH/opp/Ascend/aicpu/Ascend-aicpu_syskernels.tar.gz` |
| Kernel member | `aicpu_kernels_device/libaicpu_kernels.so`, 8,448,152 bytes |
| Architecture and entry | AArch64; `SecureDma` is a defined exported symbol |
| Package version label | `9.1.T6.0.B060`, timestamp `20260628_004128461` |
| Direct dependencies | Includes `libaicpu_sharder.so` and `libascend_hal.so`; no direct `liburma` entry |
| Extended package | Same version label; no library matching the inspector's two kernel-library name patterns |

The version label and symbol export do not identify the SecureDma source revision or prove that an active device process loaded these exact bytes. The absent direct `liburma` dependency does not establish that URMA is unavailable: another loaded library or an explicit runtime load may supply it. AICPU loader access and queue creation are still the next functional questions.

The project integration documentation identifies `scripts/sync_upstream_aicpu_to_mskernels.sh` as the source-mirroring step into the platform build. That script and the full platform `ms_kernels` build are not present in the inspected workspace. The user's online CI handles the platform build, so a local copy of that build script is not a prerequisite for preparing the probe source. CI job configuration and deployment automation have not been inspected; compilation and target execution will provide the next validation results.

The next probe should proceed in this order:

1. Run a diagnostic entry through that same AICPU execution context. First report URMA library/symbol availability, then establish initialization ownership before creating a device context/JFS/send JFC. Identify the actual device endpoint and the Host endpoint reachable from it. The reference HAL has an `ascend_urma_init` constructor that calls `urma_init`; do not assume this device process is an uninitialized standalone application or call global `urma_uninit` on state owned by its framework. Device-probe resources need their own explicit lifetime.
2. Keep a Host receiver alive with one posted slot, and pass its endpoint identity/token to the device through explicit setup data. The current Host resource probe exits and destroys its endpoint; its old queue identifiers cannot be reused.
3. SEND one small message containing a known sequence value. Require a successful device-local send completion and a successful Host receive completion with the expected receive-slot ID, byte count, and payload. The Host uses a JFC poll loop, with a bounded deadline, rather than remote state reads.
4. Establish device/native completion and transport quiescence before cleanup. Then repeat with changing sequence values and replenished receive slots before introducing graph replay.

Initially return the device diagnostic report through native task completion plus one Host copy after stream synchronization. That one-time diagnostic read is outside the eventual notification path and is not a latency measurement. Event notification and latency benchmarking are later checks; creating a JFCE alone does not test its wakeup behavior.

## AICPU symbol probe: CI and target run

The AICPU repository is on **`iter-007-zlj`**, created from `iter-007` at `5967dee`.
The probe changes are local and uncommitted; no remote branch or CI job has been published or started.
Include the working-tree changes when submitting this branch through the normal CI workflow.

The new `UMP1` diagnostic descriptor is routed through the existing `SecureDma` entry. The platform
source list includes `secure_dma_urma_probe.cc`. Its protocol header is shared directly with the Host
launcher. No Runtime source patch, new operator registration, or URMA development headers are required
for this stage. CI must include the updated `secure_dma_device.cc`, source-list fragment, and all three
new `secure_dma_urma_probe*` files from `ms_kernels/src/secure_dma/`.

If CI reports `secure_dma_test_key_binding.cc` missing, sync the corrected
`ms_kernels/secure_dma_v2.cmake`: it contains only device-source paths. The platform build should consume `SECURE_DMA_V2_SOURCES` and
`SECURE_DMA_V2_DEFINES`, with no test-binding source appended. Keep the platform's own CMake file;
the AICPU repository's root `CMakeLists.txt` only includes the device-source contract. It has no test
targets, test options, or test-configuration includes. CI needs no `test/`, `stub/`, or `cmake/` files
from this repository. The root configuration creates no build targets; device compilation remains
in the platform build. This diagnostic needs no key provider;
encrypted-copy runs still need their existing device key binding.

The user's five copy commands (entry header, `secure_dma_v2.cmake`, the complete `src/secure_dma`
directory, the two `secure_dma_kernels` files, and the JSON/INI registration files) were checked in
an isolated platform tree. All eight source-list entries and project-local includes resolve.
The platform still supplies its existing framework headers and include directories. The managed
block should append only `${SECURE_DMA_V2_SOURCES}` for this probe; the extra
`${CMAKE_CURRENT_SOURCE_DIR}/src/secure_dma_test_key_binding.cc` is not supplied by those copies.
The corrected block and architecture condition are recorded in the
[AICPU integration instructions](../../AscendCCv2-AICPU/README.md#dropping-it-into-the-platform).
Both native and compiler-path AArch64 detection were checked at CMake generation time; this is not
a completed platform build.

After CI builds and you deploy the updated `libaicpu_kernels.so` package using your usual flow, start
a fresh probe process with the reported settings:

```bash
export ASCEND_AICPU_PATH=/home/zlj/cann/cann-9.1.T560
```

Keep `SECURE_AICPU_SO_PATH` unset, as in the reported environment. The launcher uses platform dispatch
by name with the built-in AICPU scheduler, the same mode selected by Runtime's encrypted-copy dispatch.
It calls public Runtime APIs directly. `ASCEND_SECURE_MEMCPY` and its key are not required for this
diagnostic; it sends no encrypted-copy request and does not resolve a key.

On the target, place these three files together in your `jfc_notify` directory:

- [`aicpu_urma_probe.cpp`](aicpu_urma_probe.cpp)
- [`build_aicpu_probe.sh`](build_aicpu_probe.sh)
- [`secure_dma_urma_probe_protocol.h`](../../AscendCCv2-AICPU/ms_kernels/src/secure_dma/secure_dma_urma_probe_protocol.h), copied from the same AICPU revision used by CI

If the complete workspace layout is present, the builder also finds the protocol header in the sibling
AICPU checkout. Build and run on **NPU logical ID 0**:

```bash
bash build_aicpu_probe.sh
./aicpu_urma_probe --device 0 > aicpu-urma-symbols.txt 2>&1
cat aicpu-urma-symbols.txt
```

The builder locates Runtime under `$ASCEND_AICPU_PATH` (or `CANN_ROOT`). Overrides are
`RUNTIME_INCLUDE_DIR` (directory containing `runtime/kernel.h`), `RUNTIME_LIBRARY`,
`PROBE_PROTOCOL_DIR`, `AICPU_ROOT`, `CXX`, and `PROBE_OUTPUT`. Use the workload's usual Runtime/library
environment; the probe prints the library supplying `rtSetDevice` as `loaded_runtime`.

The default device probe checks 27 URMA symbols in `RTLD_DEFAULT`. If native execution succeeds but
reports missing symbols, run the optional explicit-load variant in another fresh process:

```bash
./aicpu_urma_probe --device 0 --allow-load > aicpu-urma-load.txt 2>&1
cat aicpu-urma-load.txt
```

This variant first does the same process-scope lookup, then tries `liburma.so.0`, `liburma.so`, and
`liburma.so.0.0.3` with `RTLD_NOW | RTLD_LOCAL`, stopping at the first successful open. **Loading may
execute library constructors and closing may execute destructors.** The probe releases only its own
loader reference. It never directly calls `urma_init`, `urma_uninit`, or any other URMA function, and
creates no transport resources. Existing framework-owned URMA state is not explicitly reinitialized or
uninitialized. If the first opened library lacks symbols, its result is reported without trying to mix
multiple library versions. `symbol_owner` is the owner of one resolved symbol, not an assertion that all
symbols come from that file; if owner lookup fails after an explicit open, it falls back to the soname.

Each run allocates a 640-byte device report, performs one synchronous H2D setup copy, launches one native
task, waits for stream completion, then performs one synchronous D2H report copy. There is no Host loop
that repeatedly DMA-reads a device status word. The native Runtime/driver may perform its own internal
work; this probe does not count that work or measure notification latency. `--timeout-ms` sets the
native stream wait (default 10000, maximum 30000); it is not an overall timeout for every Runtime call.

The Host checks the request prefix, completion marker, echoed cookie, architecture, and symbol mask.
Resources are freed only after confirmed native completion. If synchronization fails, the process
prints failure and exits directly without explicit buffer release, stream destruction, or Runtime
destructors that might wait again. No device reset is issued. Inspect the device log before retrying a
native failure; an old CI package without the new descriptor can reject this launch.

| Result / exit code | Meaning |
| --- | --- |
| `aicpu_urma_symbols=PASS` / 0 | Native task completed, a valid AArch64 report returned, all 27 symbols resolved, and Host cleanup succeeded |
| `aicpu_urma_symbols=UNAVAILABLE` / 3 | Valid report and cleanup; loader/symbol lookup was incomplete in this mode. This does not rule out another supported loading path |
| `aicpu_urma_symbols=FAIL` / 1 | Runtime execution, report validation, architecture check, loader-reference release, or Host cleanup failed |
| Usage error / 2 | Invalid arguments or an incompatible side-loading environment; no device work submitted |

Share the short text output. Even a PASS leaves `device_queue_creation`, `device_send`, and
`graph_replay` as **NOT_TESTED**. This proves the diagnostic ran in the selected NPU's AICPU dispatch
context; it does not identify the device's UB endpoint, establish URMA initialization ownership, prove
ABI compatibility, create a JFS, or deliver a Host CQE. Those are subsequent steps.

### Local validation of the new probe

```bash
python3 experiments/jfc_notify/tests/check_local.py
```

Run this from the workspace with both source checkouts present. It needs a C++ compiler and OpenSSL
development files, uses temporary build files, and never loads the installed driver. It compiles the
device sources in the Host configuration and runs the existing fixed, SmallCopy, missing-key, and
legacy operator suites. The launcher is linked to a
mock Runtime that validates the actual packed argument/protobuf bytes and simulates reports and failures,
including synchronization failure with no explicit early release.

These checks use CPU memory, portable Host fences, and the Host crypto backend. The production
`SECURE_DMA_DEVICE_BUILD` and AArch64 barrier guard remain intact. They do not validate AICPU framework
linking, real cache visibility, URMA provider initialization, or NPU behavior. The AICPU repository's
`test/` directory retains its existing contents from `iter-007`.

## Local preparation findings

The development workspace inspected on 2026-09-15 is x86_64, with no visible Ascend installation or NPU management tool. Local source inspection and script checks cannot provide a hardware result.

The current AICPU repository's top-level CMake exposes only the device-source contract and creates no build targets. Its `SecureDma` operator is integrated into the platform `libaicpu_kernels.so` through `ms_kernels/secure_dma_v2.cmake`. Some older Acceptance README instructions describe a separate custom-library load path, so those instructions must not be assumed to build the current official operator or provide its device URMA context.

The notification design and full experiment stages are in [the design document](../../iter-007-jfc-completion-notification-design.md).
