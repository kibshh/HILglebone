add_library(sim_spi_slave STATIC
    src/spi_slave_sensor.c
)

target_include_directories(sim_spi_slave PUBLIC inc)

target_link_libraries(sim_spi_slave PUBLIC
    shared
    protocol
    gpio
    spi_slave
    freertos_kernel
    cmsis
)
