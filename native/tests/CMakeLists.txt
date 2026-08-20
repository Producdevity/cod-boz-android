set(HOST_TESTS
    bytes_test
    gl_compat_test
    sha256_test
    source_profile_test
    config_test
    import_inventory_test
    s3e_image_test
    apk_reader_test
    dtrz_archive_test
    file_system_test
    apk_integration_test
    importer_test
    guest_memory_test
    guest_heap_test
    guest_handles_test
    guest_structs_test
    cpu_arm_test
    cpu_cross_page_test
    cpu_ordered_load_test
    cpu_import_test
    cpu_fault_test
    guest_call_test
    cpu_first_import_test
    runtime_boot_test
    runtime_audio_test
    runtime_display_test
    socket_system_test
    control_config_test
    input_mode_test
    controller_input_test
    controller_semantic_input_test
    pointer_input_test
    zeroconf_bridge_test
    codboz_controller_bridge_test
    codboz_fixups_test
    codboz_frame_interpolation_test
)

foreach(test ${HOST_TESTS})
    add_executable(${test} ${test}.cpp)
    target_compile_options(${test} PRIVATE -Wall -Wextra -Werror)
    if(test MATCHES "^(codboz_(controller_bridge|fixups|frame_interpolation)|controller|cpu|guest_call|input_mode|pointer|runtime|socket|zeroconf)_")
        target_link_libraries(${test} PRIVATE codboz_cpu)
    else()
        target_link_libraries(${test} PRIVATE codboz_loader)
    endif()
    add_test(NAME ${test} COMMAND ${test})
endforeach()
