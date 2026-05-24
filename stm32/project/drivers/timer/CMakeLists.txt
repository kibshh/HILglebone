add_library(hw_timer STATIC
    src/hw_timer.c
)

target_include_directories(hw_timer PUBLIC inc)

target_link_libraries(hw_timer PUBLIC
    shared
    cmsis
)
