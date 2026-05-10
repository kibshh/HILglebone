add_library(sim_pwm STATIC
    src/pwm_sensor.c
)

target_include_directories(sim_pwm PUBLIC inc)

target_link_libraries(sim_pwm PUBLIC
    shared
    protocol
    gpio
    hw_timer
    cmsis
)
