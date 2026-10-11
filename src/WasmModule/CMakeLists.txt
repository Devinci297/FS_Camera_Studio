# Pure module logic (no SDK calls): built natively for tests and to WASM for the simulator.
add_library(fscs_modulelib STATIC adapter.cpp coordinator.cpp input_router.cpp module_app.cpp airports.cpp)
target_include_directories(fscs_modulelib PUBLIC .)
target_link_libraries(fscs_modulelib PUBLIC fscs_core fscs_profiles fscs_protocol)
