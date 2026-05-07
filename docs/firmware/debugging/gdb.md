# Debugging the STM32 firmware with GDB + OpenOCD

End-to-end guide for stepping through the firmware on real hardware
using the Nucleo-F401RE's onboard ST-LINK. No external probe required.

## Prerequisites (host)

| Tool | Install (Ubuntu/Debian) | Purpose |
|------|-------------------------|---------|
| ARM toolchain | system package or Arm GNU Toolchain tarball | `arm-none-eabi-gcc`, build firmware |
| OpenOCD | `sudo apt install openocd` | GDB server, talks to ST-LINK |
| GDB (multi-arch) | `sudo apt install gdb-multiarch` | Debugger; `arm-none-eabi-gdb` works too if you have it from the Arm tarball |

Verify:

```bash
openocd --version
gdb-multiarch --version    # or: arm-none-eabi-gdb --version
```

## Step 1 — Build with debug symbols

The CMake config defaults to `Debug` if `CMAKE_BUILD_TYPE` isn't set
(see [stm32/CMakeLists.txt](../../../stm32/CMakeLists.txt)). Debug
flags: `-Og -g3 -gdwarf-2`.

```bash
cd stm32
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

The ELF used by GDB is `stm32/build/hilglebone-stm32.elf`.

## Step 2 — Flash the firmware

Connect the Nucleo via USB to the host (the ST-LINK enumerates as a
separate USB device — independent of the BBB connection). Then:

```bash
cmake --build build --target flash
```

This uses the `flash` custom target defined in CMakeLists.txt — under
the hood: `openocd ... -c "program ... verify reset exit"`.

## Step 3 — Start OpenOCD as a GDB server

In **terminal 1**, leave running:

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg
```

You should see, near the bottom:

```
Listening on port 3333 for gdb connections
```

The two `-f` files are bundled with OpenOCD itself
(`/usr/share/openocd/scripts/...` on Debian/Ubuntu). They tell OpenOCD
which probe (`stlink`) and which target family (`stm32f4x`) to use.

## Step 4 — Connect GDB

In **terminal 2**:

```bash
gdb-multiarch stm32/build/hilglebone-stm32.elf
```

Inside the GDB prompt, attach to OpenOCD and reset:

```gdb
target remote :3333
monitor reset halt
```

The MCU is now halted at the reset vector. Set breakpoints at the code
paths you want to inspect, then `continue`.

```gdb
break <function_name>
break <file>:<line>
continue
```

If a breakpoint fires too often (e.g. an ISR or per-byte parser entry),
disable it once you've confirmed the path is alive:

```gdb
info breakpoints                # list with numbers
disable <N>                     # silence breakpoint N
```

## GDB cheat sheet

### Navigation

| Command | Shortcut | What it does |
|---------|----------|--------------|
| `continue` | `c` | Resume until next breakpoint |
| `next` | `n` | Step over current line (don't descend) |
| `step` | `s` | Step into function calls |
| `finish` | `fin` | Run until current function returns |
| `until <line>` | `u 42` | Run until specific line |
| `nexti` / `stepi` | `ni` / `si` | One instruction at a time |
| (Enter alone) | | Repeat the previous command |

### Inspect state

| Command | What it does |
|---------|--------------|
| `print expr` / `p expr` | Show variable / expression |
| `print/x var` | Print as hex |
| `info locals` | All locals in current frame |
| `info args` | Function arguments |
| `bt` | Backtrace |
| `frame N` | Switch to frame N (from `bt`) |
| `list` | Show source around current point |
| `info breakpoints` | List all breakpoints |
| `disable N` / `enable N` / `delete N` | Manage breakpoint N |

### Peek at peripheral registers

The Cortex-M memory map exposes peripherals at fixed addresses, so you
can poke them directly from GDB. USART1 base = `0x40011000`.

```gdb
p/x *(uint32_t*)0x40011000      # USART1->SR  -- bit 7 (TXE) = TX empty
p/x *(uint32_t*)0x40011004      # USART1->DR  -- last RX byte / next TX byte
p/x *(uint32_t*)0x4001100C      # USART1->CR1 -- bit 7 (TXEIE), bit 5 (RXNEIE)
```

GPIOA base = `0x40020000`. Pin states (PA9/PA10) live in `IDR` at
offset `0x10`:

```gdb
p/x *(uint32_t*)0x40020010      # GPIOA->IDR
```

## Diagnosing by checkpoints

For any data-flow you want to debug, place a breakpoint at each stage of
the pipeline (e.g. ISR → driver buffer → parser → handler → response).
The **first stage that doesn't get hit** when you trigger the input
tells you where the chain breaks. Then drop the upstream breakpoints
and step around inside the suspect stage to find the cause.

## Detaching cleanly

```gdb
detach
quit
```

Then `Ctrl-C` in the OpenOCD terminal to stop the server. The firmware
keeps running on the MCU after detach (it doesn't get unflashed).

## Tips

- Keep the OpenOCD terminal visible while debugging — it logs target
  events (resets, halts, hard faults) that GDB doesn't always surface.
- If GDB seems unresponsive after `continue`, you may be hitting a
  noisy breakpoint per byte. Press `Ctrl-C` to break in, then disable
  it.
- A hard fault leaves the PC at the fault handler. Check
  `bt` — if you see `HardFault_Handler` in the trace, it's likely a
  null pointer dereference, stack overflow, or unaligned access. Look
  at the LR / MSP values to find the offending code.
- The `-Og` optimisation level keeps most variables in memory (not
  optimised into registers) so `print` works as expected on locals.
