add_library(sim_dac STATIC
    src/dac_sensor.c
)

target_include_directories(sim_dac PUBLIC inc)

target_link_libraries(sim_dac PUBLIC
    shared
    protocol
    gpio
    spi_master
    freertos_kernel
    cmsis
)
