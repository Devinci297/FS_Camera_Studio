add_library(fscs_profiles STATIC src/json_util.cpp src/profile.cpp src/bindings.cpp src/repository.cpp)
target_include_directories(fscs_profiles PUBLIC include)
target_link_libraries(fscs_profiles PUBLIC fscs_core fscs_json)
