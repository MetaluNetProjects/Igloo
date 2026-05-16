add_library(partition_info INTERFACE)

target_sources(partition_info INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/uf2_family_ids.c
    ${CMAKE_CURRENT_SOURCE_DIR}/partition_info.c
)

target_include_directories(partition_info INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(partition_info INTERFACE
    pico_stdlib
    #pico_sha256
    #pico_multicore
    boot_uf2_headers
    #pico_unique_id
    pico_bootrom
    hardware_flash
)

