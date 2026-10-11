add_library(fscs_protocol STATIC src/protocol.cpp)
target_include_directories(fscs_protocol PUBLIC include)
target_link_libraries(fscs_protocol PUBLIC fscs_profiles fscs_json)
