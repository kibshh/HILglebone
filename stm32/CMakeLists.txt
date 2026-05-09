cmake_minimum_required(VERSION 3.20)

# ─── Toolchain (must be set before project()) ───
# Invoke: cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
# Optionally override target: -DSTM32_MCU=STM32F411RE

project(hilglebone-stm32
    VERSION 0.1.0
    LANGUAGES C ASM
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Target configuration ───
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32_target.cmake)

# ─── Clock source selection ───
option(USE_HSE "Use external HSE oscillator instead of internal HSI" ON)

# ─── CMSIS vendor paths ───
set(CMSIS_CORE_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/project/vendor/cmsis_core/CMSIS/Core/Include)
set(CMSIS_DEVICE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/project/vendor/cmsis_device_${STM32_FAMILY_LOWER}/Include)
set(CMSIS_STARTUP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/project/vendor/cmsis_device_${STM32_FAMILY_LOWER}/Source/Templates/gcc)
set(CMSIS_SYSTEM_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/project/vendor/cmsis_device_${STM32_FAMILY_LOWER}/Source/Templates)

# ─── INTERFACE library: CMSIS headers ───
add_library(cmsis INTERFACE)
target_include_directories(cmsis INTERFACE
    ${CMSIS_CORE_DIR}
    ${CMSIS_DEVICE_DIR}
)
target_compile_definitions(cmsis INTERFACE
    ${STM32_COMPILE_DEFINITIONS}
    $<$<BOOL:${USE_HSE}>:USE_HSE>
)

# ─── FreeRTOS kernel library ───
set(FREERTOS_DIR      ${CMAKE_CURRENT_SOURCE_DIR}/project/vendor/freertos_kernel)
set(FREERTOS_PORT_DIR ${FREERTOS_DIR}/portable/GCC/ARM_CM4F)

add_library(freertos_kernel STATIC
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
    ${FREERTOS_PORT_DIR}/port.c
)
target_include_directories(freertos_kernel PUBLIC
    ${FREERTOS_DIR}/include
    ${FREERTOS_PORT_DIR}
)
target_link_libraries(freertos_kernel PUBLIC cmsis)

# ─── Compiler flags ───
set(COMMON_FLAGS "${STM32_CPU_FLAGS} -Wall -Wextra -Werror")
set(COMMON_FLAGS "${COMMON_FLAGS} -ffunction-sections -fdata-sections -fno-common")
set(COMMON_FLAGS "${COMMON_FLAGS} -specs=nano.specs -specs=nosys.specs")

set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${COMMON_FLAGS}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${COMMON_FLAGS}")

# Release: optimise for size.  Debug: no optimisation, debug symbols.
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG   "-Og -g3 -gdwarf-2")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ─── Components ───
add_subdirectory(project/shared)
add_subdirectory(project/drivers)
add_subdirectory(project/protocol)
add_subdirectory(project/simulate)
add_subdirectory(project/command)
add_subdirectory(project/app)

# FreeRTOS needs FreeRTOSConfig.h from shared/inc.
target_link_libraries(freertos_kernel PUBLIC shared)

# ─── Linker flags ───
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/STM32_FLASH.ld)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -T${LINKER_SCRIPT}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--print-memory-usage")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=FLASH_ORIGIN=${STM32_FLASH_ORIGIN}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=FLASH_SIZE=${STM32_FLASH_SIZE}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=RAM_ORIGIN=${STM32_RAM_ORIGIN}")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--defsym=RAM_SIZE=${STM32_RAM_SIZE}")

# ─── Executable ───
set(STARTUP_FILE ${CMSIS_STARTUP_DIR}/${STM32_STARTUP})

add_executable(${PROJECT_NAME}
    ${STARTUP_FILE}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    app
    command
    simulate
    protocol
    uart
    gpio
    shared
    freertos_kernel
    cmsis
)

set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX .elf)

# ─── Post-build: generate .bin and .hex, print size ───
set(ELF_FILE $<TARGET_FILE:${PROJECT_NAME}>)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   ${ELF_FILE} $<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary ${ELF_FILE} $<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE}    ${ELF_FILE}
    COMMENT "Generating .hex and .bin, printing size"
)

# ─── Flash target (OpenOCD) ───
add_custom_target(flash
    COMMAND openocd
        -f interface/stlink.cfg
        -f target/stm32${STM32_FAMILY_LOWER}x.cfg
        -c "program ${ELF_FILE} verify reset exit"
    DEPENDS ${PROJECT_NAME}
    COMMENT "Flashing ${STM32_MCU} via ST-Link"
)
