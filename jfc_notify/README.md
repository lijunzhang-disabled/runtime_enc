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

## Next two functional checks

1. **Host receive resources:** compile against the target's matching URMA headers/library; enumerate device/EID choices, explicitly select one, create a dedicated JFCE/JFC/JFR, register and post a receive buffer, and cleanly tear down through the supported API. Do not borrow the synchronous-copy driver's pooled queues.
2. **Device SEND:** establish the matching device endpoint in the actual intended execution context, send one synthetic message, validate the Host receive CQE/message, consume the device's local send completion, and release resources safely. Then repeat enough messages to exercise receive replenishment.

Only after these checks pass should we add capture/replay and then secure-copy integration. A SEND from a generic device process does not establish that the SecureDma AICPU execution process can submit it.

## Local preparation findings

The development workspace inspected on 2026-09-15 is x86_64, with no visible Ascend installation or NPU management tool. Local source inspection and script checks cannot provide a hardware result.

The current AICPU repository's top-level CMake builds Host tests. Its `SecureDma` operator is integrated into the platform `libaicpu_kernels.so` through `ms_kernels/secure_dma_v2.cmake`. Some older Acceptance README instructions describe a separate custom-library load path, so those instructions must not be assumed to build the current official operator or provide its device URMA context.

The notification design and full experiment stages are in [the design document](../../iter-007-jfc-completion-notification-design.md).
