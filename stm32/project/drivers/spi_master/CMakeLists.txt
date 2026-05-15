add_library(spi_master STATIC
    src/spi_master.c
)

target_include_directories(spi_master PUBLIC inc)

target_link_libraries(spi_master PUBLIC
    shared
    cmsis
)
