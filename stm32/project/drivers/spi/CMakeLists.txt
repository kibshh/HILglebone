add_library(spi STATIC
    src/spi.c
)

target_include_directories(spi PUBLIC inc)

target_link_libraries(spi PUBLIC
    shared
    cmsis
)
