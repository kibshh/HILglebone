# STM32 Target Configuration
#
# Maps an STM32 part number to its CPU core, FPU, flash/RAM sizes, and CMSIS defines.
# Set STM32_MCU before including this file (default: STM32F401RE).
#
# To add a new chip:
#   1. Add an elseif() block below with the chip's parameters
#   2. If it's a new family (not F4), you also need the matching CMSIS device headers

if(NOT STM32_MCU)
    set(STM32_MCU "STM32F401RE" CACHE STRING "Target STM32 MCU part number")
endif()

message(STATUS "STM32 target: ${STM32_MCU}")

# --- Extract family from part number (e.g., STM32F401RE -> F4) ---
string(SUBSTRING ${STM32_MCU} 5 2 STM32_FAMILY)
string(TOUPPER ${STM32_FAMILY} STM32_FAMILY)
string(TOLOWER ${STM32_FAMILY} STM32_FAMILY_LOWER)

# --- Per-chip configuration ---

if(STM32_MCU STREQUAL "STM32F401RE")
    set(STM32_CPU          "cortex-m4")
    set(STM32_FPU          "fpv4-sp-d16")
    set(STM32_FLOAT_ABI    "hard")
    set(STM32_FLASH_SIZE   "512K")
    set(STM32_RAM_SIZE     "96K")
    set(STM32_FLASH_ORIGIN "0x08000000")
    set(STM32_RAM_ORIGIN   "0x20000000")
    set(STM32_CMSIS_DEVICE "stm32f401xe")
    set(STM32_HAL_DEFINE   "STM32F401xE")
    set(STM32_STARTUP      "startup_stm32f401xe.s")

elseif(STM32_MCU STREQUAL "STM32F411RE")
    set(STM32_CPU          "cortex-m4")
    set(STM32_FPU          "fpv4-sp-d16")
    set(STM32_FLOAT_ABI    "hard")
    set(STM32_FLASH_SIZE   "512K")
    set(STM32_RAM_SIZE     "128K")
    set(STM32_FLASH_ORIGIN "0x08000000")
    set(STM32_RAM_ORIGIN   "0x20000000")
    set(STM32_CMSIS_DEVICE "stm32f411xe")
    set(STM32_HAL_DEFINE   "STM32F411xE")
    set(STM32_STARTUP      "startup_stm32f411xe.s")

elseif(STM32_MCU STREQUAL "STM32F446RE")
    set(STM32_CPU          "cortex-m4")
    set(STM32_FPU          "fpv4-sp-d16")
    set(STM32_FLOAT_ABI    "hard")
    set(STM32_FLASH_SIZE   "512K")
    set(STM32_RAM_SIZE     "128K")
    set(STM32_FLASH_ORIGIN "0x08000000")
    set(STM32_RAM_ORIGIN   "0x20000000")
    set(STM32_CMSIS_DEVICE "stm32f446xx")
    set(STM32_HAL_DEFINE   "STM32F446xx")
    set(STM32_STARTUP      "startup_stm32f446xx.s")

elseif(STM32_MCU STREQUAL "STM32F407VG")
    set(STM32_CPU          "cortex-m4")
    set(STM32_FPU          "fpv4-sp-d16")
    set(STM32_FLOAT_ABI    "hard")
    set(STM32_FLASH_SIZE   "1024K")
    set(STM32_RAM_SIZE     "128K")
    set(STM32_FLASH_ORIGIN "0x08000000")
    set(STM32_RAM_ORIGIN   "0x20000000")
    set(STM32_CMSIS_DEVICE "stm32f407xx")
    set(STM32_HAL_DEFINE   "STM32F407xx")
    set(STM32_STARTUP      "startup_stm32f407xx.s")

else()
    message(FATAL_ERROR
        "Unsupported STM32_MCU: ${STM32_MCU}\n"
        "Add a configuration block in cmake/stm32_target.cmake for this chip."
    )
endif()

# --- Derived compiler flags ---

set(STM32_CPU_FLAGS "-mcpu=${STM32_CPU} -mthumb")

if(STM32_FPU)
    string(APPEND STM32_CPU_FLAGS " -mfpu=${STM32_FPU} -mfloat-abi=${STM32_FLOAT_ABI}")
endif()

set(STM32_COMPILE_DEFINITIONS ${STM32_HAL_DEFINE})

message(STATUS "  CPU: ${STM32_CPU}, FPU: ${STM32_FPU}")
message(STATUS "  Flash: ${STM32_FLASH_SIZE} @ ${STM32_FLASH_ORIGIN}")
message(STATUS "  RAM:   ${STM32_RAM_SIZE} @ ${STM32_RAM_ORIGIN}")
message(STATUS "  CMSIS device: ${STM32_CMSIS_DEVICE}")
