add_library(sim_digital_out STATIC
    src/digital_out_sensor.c
    src/digital_out_pulse_timer.c
)

target_include_directories(sim_digital_out PUBLIC inc)

target_link_libraries(sim_digital_out PUBLIC
    shared
    protocol
    gpio
    freertos_kernel
    cmsis
)
