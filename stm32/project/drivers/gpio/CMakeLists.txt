add_library(gpio STATIC
    src/gpio.c
)

target_include_directories(gpio PUBLIC inc)

target_link_libraries(gpio PUBLIC
    shared
    cmsis
)
