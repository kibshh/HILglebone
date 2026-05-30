add_library(i2c_slave STATIC src/i2c_slave.c)

target_include_directories(i2c_slave PUBLIC inc)

target_link_libraries(i2c_slave PUBLIC shared cmsis)
