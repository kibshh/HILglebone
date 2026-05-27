add_library(spi_slave STATIC
    src/spi_slave.c
)

target_include_directories(spi_slave PUBLIC inc)

target_link_libraries(spi_slave PUBLIC
    shared
    spi
    cmsis
)
