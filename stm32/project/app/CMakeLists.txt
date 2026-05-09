add_library(app STATIC
    src/main.c
    src/app_init.c
    src/clock.c
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
