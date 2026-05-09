add_library(uart STATIC
    src/uart.c
)

target_include_directories(uart PUBLIC inc)

target_link_libraries(uart PUBLIC
    shared
    freertos_kernel
    cmsis
)
