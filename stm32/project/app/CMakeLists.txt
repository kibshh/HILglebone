add_library(app STATIC
    src/main.c
    src/app_init.c
    src/clock.c
    ${CMSIS_SYSTEM_DIR}/system_stm32${STM32_FAMILY_LOWER}xx.c
)

target_include_directories(app PUBLIC inc)

target_link_libraries(app PUBLIC
    shared
    drivers
    protocol
    command
    freertos_kernel
    cmsis
)
