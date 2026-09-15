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

This is enough to build the Host resource probe. The library filename does not establish a package release or header/library ABI match. Queue creation, actual provider loading, AICPU access, and notification delivery remain untested. The directory permission errors make the filesystem inventory incomplete, but the required Host header/core/provider candidates were found. No `libaicpu_kernels.so` candidate appeared in this search; that does not establish its absence from the device environment.

The subsequent target enumeration **passed**: URMA initialization, device/EID enumeration, and uninitialization succeeded. It returned ten devices, `udmac{0,1}d1e{2,3,4,5,6}`. All have EID index 0; each `e6` device has nine EIDs. Queue creation and delivery remain untested. Version 1 reported the executable as `loaded_urma`; this diagnostic bug is reproducible locally with a non-PIE build, where the function address identifies a PLT entry. Version 2 resolves the symbol definition through `dlsym(RTLD_NEXT, ...)` before inspecting its owner.

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

Local validation uses the source-reference headers and a mock URMA library. It checks compilation, command-line validation, call order, and failure cleanup; the target build and real hardware behavior still need the above runs. Queue configuration follows the driver's HDC JFR example and HCOMM's local-only segment registration, without copying their application-specific memory allocator or remote endpoint setup.

## Next two functional checks

1. **Host receive resources:** compile against the target's matching URMA headers/library; enumerate device/EID choices, explicitly select one, create a dedicated JFCE/JFC/JFR, register and post a receive buffer, and cleanly tear down through the supported API. Do not borrow the synchronous-copy driver's pooled queues.
2. **Device SEND:** establish the matching device endpoint in the actual intended execution context, send one synthetic message, validate the Host receive CQE/message, consume the device's local send completion, and release resources safely. Then repeat enough messages to exercise receive replenishment.

Only after these checks pass should we add capture/replay and then secure-copy integration. A SEND from a generic device process does not establish that the SecureDma AICPU execution process can submit it.

## Local preparation findings

The development workspace inspected on 2026-09-15 is x86_64, with no visible Ascend installation or NPU management tool. Local source inspection and script checks cannot provide a hardware result.

The current AICPU repository's top-level CMake builds Host tests. Its `SecureDma` operator is integrated into the platform `libaicpu_kernels.so` through `ms_kernels/secure_dma_v2.cmake`. Some older Acceptance README instructions describe a separate custom-library load path, so those instructions must not be assumed to build the current official operator or provide its device URMA context.

The notification design and full experiment stages are in [the design document](../../iter-007-jfc-completion-notification-design.md).
