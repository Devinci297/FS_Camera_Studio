add_executable(fscs_tests main.cpp test_core.cpp test_profiles.cpp test_coordinator.cpp test_module.cpp test_storage.cpp test_airports.cpp
    ../src/WasmModule/wasm/wasm_storage.cpp)
target_link_libraries(fscs_tests PRIVATE fscs_modulelib)
target_include_directories(fscs_tests PRIVATE .)
add_test(NAME fscs_tests COMMAND fscs_tests)

add_executable(fscs_export_c172 ../tools/export_c172.cpp)
target_link_libraries(fscs_export_c172 PRIVATE fscs_profiles)
