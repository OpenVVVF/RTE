# Toolchain — graph to firmware

How a node graph becomes a flashable STM32 image.

```
 ┌─────────────────────┐
 │  NodeGUI / editor   │  edit Assets/Examples/*.json
 └──────────┬──────────┘
            │ NodeAPI graph JSON
            ▼
 ┌─────────────────────┐
 │  InverterCodegen    │  one .h/.cpp pair per timing domain
 └──────────┬──────────┘
            │ generated domain sources
            ▼
 ┌─────────────────────┐
 │  RTECodeEmitter     │  copy Images/Gen6FW, splice at // RTE_EMIT: markers
 └──────────┬──────────┘
            │ output firmware tree
            ▼
 ┌─────────────────────┐
 │  RTEFirmwareBuilder │  arm-none-eabi CMake build
 └──────────┬──────────┘
            │
            ▼
      STM32CubeMX.elf / .bin
```

`RTEFirmwareBuilder` can run the emitter step for you; you can also invoke each
stage separately while debugging.

## 1. Author the graph

- Prefer editing in **NodeGUI** (`docs/NodeGUI.md`).
- Node types live under `Assets/NodeTemplates/<type-id>/` (see
  `docs/NodeTemplates.md`).
- Assign every node a **timing domain** that matches a base-image ISR / loop
  (`adc_isr`, `tim_isr`, `app_loop`, …).
- Use **bridges** for cross-domain signals.

Validate early with NodeAPI tests or by loading in NodeGUI (rejected wires
surface in the status bar).

## 2. Generate domain C++ (optional standalone)

```bash
./build/Lib/InverterCodegen/InverterCodegen \
  --graph Assets/Examples/foc_chain.json \
  --output /tmp/codegen_out \
  --templates Assets/NodeTemplates
```

See `Lib/InverterCodegen/README.md` for quantity/unit conventions (`RteQuantity.h`
+ Au units).

## 3. Emit into a base firmware tree

```bash
./build/Source/RTECodeEmitter/RTECodeEmitter \
  --base-src Images/Gen6FW \
  --graph    Assets/Examples/foc_chain.json \
  --output   /tmp/foc_out \
  --templates Assets/NodeTemplates
```

Markers in the base image look like:

```cpp
// RTE_EMIT: tim_isr state
// RTE_EMIT: tim_isr init
// RTE_EMIT: tim_isr step
```

Full marker reference: `Source/RTECodeEmitter/README.md`.

## 4. Build the STM32 binary

Install the ARM GNU toolchain if needed:

```bash
./scripts/install_stm32_toolchain.sh
```

Then:

```bash
./build/Source/RTEFirmwareBuilder/RTEFirmwareBuilder \
    --fw-src    Images/Gen6FW \
    --build-dir build/rtetest-fw \
    --graph     Images/Gen6FW/baseline_graph.json \
    --base-src  Images/Gen6FW \
    --output    build/rtetest-fw-src \
    --verbosity info
```

Outputs (typical):

```
build/rtetest-fw/
├── STM32CubeMX.elf
└── STM32CubeMX.bin
```

## Libraries involved

| Library | Responsibility |
|---|---|
| `NodeAPI` | Graph IR, JSON, wire typing, timing/DAG validation |
| `InverterCodegen` | Domain C++ emission |
| `InverterProtocol` | Host↔device telemetry/command framing (COBS + CRC) |
| `RTELogger` | Shared host logging |

Device firmware base image: `Images/Gen6FW` (STM32H723). Safety coprocessor
firmware currently still ships alongside Hardware
(`CoprocessorFW` in [OpenVVVF/Hardware](https://github.com/OpenVVVF/Hardware)).

## Design constraints worth remembering

- Generated code avoids heap, exceptions, and templates in the **output**.
- Timing domains are first-class; codegen never silently mixes ISR rates.
- Bridges are explicit — no “magic” globals between domains except what codegen
  emits for a bridge.
- The base image must keep compiling with markers left as comments (dry-run /
  no-emit friendly).
