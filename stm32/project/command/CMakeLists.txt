add_library(command STATIC
    src/dispatcher.c
    src/sensor_manager.c
)

target_include_directories(command PUBLIC inc)

target_link_libraries(command PUBLIC
    shared
    protocol
    sim_i2c
    sim_digital_out
    freertos_kernel
    cmsis
)
