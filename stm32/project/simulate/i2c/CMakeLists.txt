add_library(sim_i2c STATIC
    src/i2c_sensor.c
)

target_include_directories(sim_i2c PUBLIC inc)

target_link_libraries(sim_i2c PUBLIC
    shared
    protocol
    gpio
    i2c_slave
    freertos_kernel
    cmsis
)
