add_library(protocol STATIC
    src/crc16.c
    src/protocol_encoder.c
    src/protocol_parser.c
)

target_include_directories(protocol PUBLIC inc)

target_link_libraries(protocol PUBLIC
    shared
    drivers
    cmsis
)
