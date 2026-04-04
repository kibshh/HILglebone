# CMake toolchain file for ARM Cortex-M bare-metal cross-compilation
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake ..

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Find the toolchain — optionally override with -DARM_TOOLCHAIN_PATH=/path/to/bin
find_program(ARM_GCC arm-none-eabi-gcc HINTS ${ARM_TOOLCHAIN_PATH} ENV PATH)
if(NOT ARM_GCC)
    message(FATAL_ERROR
        "arm-none-eabi-gcc not found.\n"
        "Install it:  sudo apt install gcc-arm-none-eabi\n"
        "Or pass:     -DARM_TOOLCHAIN_PATH=/path/to/gcc-arm-none-eabi-XX/bin"
    )
endif()

set(TOOLCHAIN_PREFIX arm-none-eabi-)
get_filename_component(TOOLCHAIN_BIN_DIR ${ARM_GCC} DIRECTORY)

set(CMAKE_C_COMPILER   ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_ASM_COMPILER ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}gcc)
set(CMAKE_OBJCOPY      ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objcopy)
set(CMAKE_OBJDUMP      ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}objdump)
set(CMAKE_SIZE         ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN_PREFIX}size)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
