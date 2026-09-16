# JFC notification experiment: first step

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

The user selected **NPU logical ID 0** for the next probe. Its UB endpoint mapping is still unverified. The next implementation depends on the build/deployment command for the currently used `libaicpu_kernels.so` and its deployed or side-loaded location. The local AICPU repository's top-level CMake builds Host tests only; it cannot supply a deployable operator binary. Runtime's `Dispatch` supports both platform dispatch by name and `SECURE_AICPU_SO_PATH` side loading, with built-in AICPU scheduler selection in both cases. Use the mode already exercised by the encrypted-copy workload.

To locate the current artifact, first inspect `SECURE_AICPU_SO_PATH` in the workload's launch environment. If unset, Runtime dispatches `SecureDma` by name; the Host installation may contain an AICPU archive instead of an unpacked `.so`. The inspected TSD package loader uses `ASCEND_AICPU_PATH` (default `/usr/local/Ascend/`) and an `opp/<package-title>/aicpu/` directory containing an `*aicpu_syskernels.tar.gz` package. Device-side extraction can use `/usr/lib64/aicpu_kernels/<uniqueVfId>/aicpu_kernels_device/` or another run-mode-specific path; that path need not be visible on the Host, and `uniqueVfId` must not be assumed to equal the NPU logical ID. These source-derived locations are search candidates until confirmed on the target.

The user found these Host package candidates:

- `/home/zlj/cann/cann/opp/Ascend/aicpu/Ascend-aicpu_syskernels.tar.gz`
- `/home/zlj/cann/cann/opp/Ascend/aicpu/Ascend-aicpu_extend_syskernels.tar.gz`

On 2026-09-16 the user confirmed `SECURE_AICPU_SO_PATH` is unset and `ASCEND_AICPU_PATH=/home/zlj/cann/cann-9.1.T560`. Runtime therefore uses platform dispatch by name in that environment. Whether the earlier `/home/zlj/cann/cann` path resolves to this same installation remains unconfirmed. Running `tar -tzf` on the earlier package failed with `not in gzip format`; its suffix alone does not identify the outer file format.

The inspected Runtime TSD reader handles a CMS package with an 8192-byte header and a 256-byte descriptor before the archive payload. This is a candidate explanation, not a confirmed format for the user's file. See `PackageVerify::ProcessSendStepVerify` in `/home/zlj/ascend_stack/runtime/src/tsd/basic_component/package_manager/src/package_verify.cpp`.

Copy `inspect_aicpu_package.py` to the target and run it in the workload's launch environment:

```bash
python3 inspect_aicpu_package.py
```

It reads both packages under `$ASCEND_AICPU_PATH/opp/Ascend/aicpu/`, prints their resolved paths, and checks for an ordinary archive or an archive at the source-reference payload offset. It reports at most four matching kernel-library members per package, ELF architecture, whether `SecureDma` is a defined exported symbol, a short dependency list, and version-file summaries. It uses Python's standard library and `readelf`; no `rg` is required. To inspect another candidate, use `--package /path/to/package` (repeatable).

The inspector opens installed packages read-only and copies selected regular members into temporary files for `readelf`; it never loads a library, changes an installed package, or follows archived symlinks. It does not authenticate a signature. An unknown format is reported with short byte prefixes; it is not guessed or rewritten. Library and total archive inspection have size limits, with incomplete inspection reported explicitly. Output is static package evidence, not proof that a particular device process loaded the binary or can use URMA.

Their library contents, the `SecureDma` export, and use by the active workload still need target inspection. An absent direct `liburma` dependency would not rule out runtime loading; device-context functionality still requires the planned AICPU probe.

The next probe should proceed in this order:

1. Run a diagnostic entry through that same AICPU execution context. Report whether its process can resolve the deployed URMA API and create a device context/JFS/send JFC. Identify the actual device endpoint and the Host endpoint reachable from it.
2. Keep a Host receiver alive with one posted slot, and pass its endpoint identity/token to the device through explicit setup data. The current Host resource probe exits and destroys its endpoint; its old queue identifiers cannot be reused.
3. SEND one small message containing a known sequence value. Require a successful device-local send completion and a successful Host receive completion with the expected receive-slot ID, byte count, and payload. The Host uses a JFC poll loop, with a bounded deadline, rather than remote state reads.
4. Establish device/native completion and transport quiescence before cleanup. Then repeat with changing sequence values and replenished receive slots before introducing graph replay.

Initially return the device diagnostic report through native task completion plus one Host copy after stream synchronization. That one-time diagnostic read is outside the eventual notification path and is not a latency measurement. Event notification and latency benchmarking are later checks; creating a JFCE alone does not test its wakeup behavior.

## Local preparation findings

The development workspace inspected on 2026-09-15 is x86_64, with no visible Ascend installation or NPU management tool. Local source inspection and script checks cannot provide a hardware result.

The current AICPU repository's top-level CMake builds Host tests. Its `SecureDma` operator is integrated into the platform `libaicpu_kernels.so` through `ms_kernels/secure_dma_v2.cmake`. Some older Acceptance README instructions describe a separate custom-library load path, so those instructions must not be assumed to build the current official operator or provide its device URMA context.

The notification design and full experiment stages are in [the design document](../../iter-007-jfc-completion-notification-design.md).
