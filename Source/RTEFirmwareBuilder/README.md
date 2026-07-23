# RTEFirmwareBuilder

`RTEFirmwareBuilder` is a distro-agnostic wrapper that configures and builds the
STM32 firmware in `Images/Gen6FW`. It handles toolchain detection, optional
`RTECodeEmitter` integration, and CMake invocation with structured logging.

## Where it sits in the pipeline

```
Node graph (JSON) ──┐
Base firmware ──────┼──► RTECodeEmitter ──► RTEFirmwareBuilder ──► .elf / .bin
                     (optional)                 ▲
                                               │
                                    auto-detects ARM toolchain
```

## Build the RTE tools

```bash
cmake -B build -G Ninja
cmake --build build -j8
```

This produces `build/Source/RTEFirmwareBuilder/RTEFirmwareBuilder`.

## Quick start: build the baseline firmware

A minimal baseline graph is provided at `Images/Gen6FW/baseline_graph.json`.
It creates empty `app_loop`, `tim_isr`, and `adc_isr` domains so the base
firmware compiles without any application logic.

```bash
./build/Source/RTEFirmwareBuilder/RTEFirmwareBuilder \
    --fw-src    Images/Gen6FW \
    --build-dir build/rtetest-fw \
    --graph     Images/Gen6FW/baseline_graph.json \
    --base-src  Images/Gen6FW \
    --output    build/rtetest-fw-src \
    --verbosity info
```

After a successful build you will have:

```
build/rtetest-fw/
├── STM32CubeMX.elf
└── STM32CubeMX.bin
```

## CLI

```bash
RTEFirmwareBuilder \
    --fw-src <dir>          # firmware source directory (contains CMakeLists.txt)
    --build-dir <dir>       # CMake build output directory
    [--toolchain <mode>]    # auto | gcc | starm-clang | <path> (default: auto)
    [--build-type <type>]   # Debug | Release (default: Debug)
    [--generator <gen>]     # CMake generator (default: Ninja)
    [--graph <file>]        # NodeAPI graph JSON (requires --base-src and --output)
    [--base-src <dir>]      # base firmware source for emitter
    [--output <dir>]        # emitted firmware source directory
    [--clean]               # delete build-dir before configuring
    [--verbosity <level>]   # error | warning | info | debug | trace
    [--dry-run]             # print commands without executing
    [--help]
```

## Toolchain setup

If the wrapper cannot find `arm-none-eabi-gcc` and `arm-none-eabi-g++`, it will
print an error with install instructions. The easiest cross-distro option is to
use the bundled install script:

```bash
./scripts/install_stm32_toolchain.sh
```

This downloads the xPack GNU Arm Embedded GCC into `.tools/` under the project
root. `RTEFirmwareBuilder` will detect it automatically.

You can also install via your package manager:

- Debian/Ubuntu: `sudo apt install gcc-arm-none-eabi`
- Fedora: `sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-gcc-cs-c++`
- Arch: `sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib`

## Examples

Build-only (firmware source is already emitted):

```bash
RTEFirmwareBuilder --fw-src build/rtetest-fw-src --build-dir build/rtetest-fw
```

Use a custom toolchain file:

```bash
RTEFirmwareBuilder \
    --fw-src Images/Gen6FW \
    --build-dir build/rtetest-fw \
    --toolchain Images/Gen6FW/cmake/gcc-arm-none-eabi.cmake
```

Full graph → binary pipeline:

```bash
RTEFirmwareBuilder \
    --fw-src    Images/Gen6FW \
    --build-dir build/rtetest-fw \
    --graph     Images/Gen6FW/baseline_graph.json \
    --base-src  Images/Gen6FW \
    --output    build/rtetest-fw-src \
    --clean
```

## Notes

- The wrapper prepends the detected toolchain `bin/` directory to `PATH` for the
  CMake subprocess so that `arm-none-eabi-g++` is resolved consistently.
- Build output is streamed at `debug` verbosity; use `--verbosity debug` to see
  compiler messages.
- Flashing is intentionally not part of this tool. Use `flash_uart.py`,
  `STM32CubeProgrammer`, or OpenOCD after a successful build.
