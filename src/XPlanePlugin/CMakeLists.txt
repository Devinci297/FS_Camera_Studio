# XP Camera Studio: the X-Plane 12 plugin (doc: X-Plane plan). Built only with -DFSCS_BUILD_XPLANE=ON, so the MSFS
# build and its tests are unchanged. Targets X-Plane 12.4.4 and newer (SDK 4.4.0, XPLM440): its browser windows host
# the shared ui/.

set(FSCS_XPSDK_DIR "${PROJECT_SOURCE_DIR}/third_party/XPSDK" CACHE PATH "X-Plane SDK root (CHeaders, Libraries)")
if(NOT EXISTS "${FSCS_XPSDK_DIR}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR "X-Plane SDK not found at ${FSCS_XPSDK_DIR}")
endif()
set(XPCS_VERSION "0.1.0" CACHE STRING "XP Camera Studio version")

# The module libraries are linked into a shared plugin.
set_target_properties(fscs_core fscs_profiles fscs_protocol fscs_modulelib PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(xpcs_plugin MODULE plugin_main.cpp xp_airports.cpp xp_camera.cpp xp_mouse.cpp xp_panels.cpp xp_storage.cpp
    xp_traffic.cpp)
find_package(Threads REQUIRED)  # the apt.dat index runs on a worker thread
target_link_libraries(xpcs_plugin PRIVATE fscs_modulelib fscs_json Threads::Threads)
target_include_directories(xpcs_plugin SYSTEM PRIVATE "${FSCS_XPSDK_DIR}/CHeaders/XPLM")
target_compile_definitions(xpcs_plugin PRIVATE XPCS_VERSION="${XPCS_VERSION}"
    # Every API level up to 4.4.0 (the macros are not cumulative).
    XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1 XPLM410=1 XPLM411=1 XPLM420=1 XPLM430=1 XPLM440=1)

# X-Plane loads <plugin folder>/<platform>/<name>.xpl; the UI goes to <plugin folder>/ui.
set(XPCS_PLUGIN_DIR "${CMAKE_BINARY_DIR}/XPCameraStudio")
if(WIN32)
    target_compile_definitions(xpcs_plugin PRIVATE IBM=1)
    target_link_libraries(xpcs_plugin PRIVATE "${FSCS_XPSDK_DIR}/Libraries/Win/XPLM_64.lib")
    set(XPCS_PLATFORM_DIR win_x64)
elseif(APPLE)
    message(FATAL_ERROR "Mac builds come later: add the SDK frameworks to third_party/XPSDK first")
else()
    # Linux: XPLM symbols stay undefined and are resolved by X-Plane at load time.
    target_compile_definitions(xpcs_plugin PRIVATE LIN=1)
    # Export only the XPlugin entry points: other plugins may carry their own copies of the same libraries.
    set_target_properties(xpcs_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    target_link_options(xpcs_plugin PRIVATE "LINKER:--exclude-libs,ALL")
    set(XPCS_PLATFORM_DIR lin_x64)
endif()

set_target_properties(xpcs_plugin PROPERTIES
    OUTPUT_NAME XPCameraStudio
    PREFIX ""
    SUFFIX ".xpl"
    LIBRARY_OUTPUT_DIRECTORY "${XPCS_PLUGIN_DIR}/${XPCS_PLATFORM_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${XPCS_PLUGIN_DIR}/${XPCS_PLATFORM_DIR}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${XPCS_PLUGIN_DIR}/${XPCS_PLATFORM_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${XPCS_PLUGIN_DIR}/${XPCS_PLATFORM_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${XPCS_PLUGIN_DIR}/${XPCS_PLATFORM_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${XPCS_PLUGIN_DIR}/${XPCS_PLATFORM_DIR}")

# The shared UI, without its tests, next to the plugin binary.
add_custom_command(TARGET xpcs_plugin POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${XPCS_PLUGIN_DIR}/ui"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${PROJECT_SOURCE_DIR}/ui" "${XPCS_PLUGIN_DIR}/ui"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${XPCS_PLUGIN_DIR}/ui/tests"
    VERBATIM)

# Linux only: a stand-in X-Plane that loads the built plugin and plays a browser window (src/XPlanePlugin/tests).
if(UNIX AND NOT APPLE)
    add_executable(xpcs_fake_xplane tests/fake_xplane.cpp)
    target_link_libraries(xpcs_fake_xplane PRIVATE fscs_protocol fscs_json ${CMAKE_DL_LIBS})
    target_include_directories(xpcs_fake_xplane SYSTEM PRIVATE "${FSCS_XPSDK_DIR}/CHeaders/XPLM")
    target_compile_definitions(xpcs_fake_xplane PRIVATE LIN=1
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1 XPLM410=1 XPLM411=1 XPLM420=1 XPLM430=1 XPLM440=1)
    set_target_properties(xpcs_fake_xplane PROPERTIES ENABLE_EXPORTS ON)  # the plugin resolves XPLM* from it
    add_dependencies(xpcs_fake_xplane xpcs_plugin)
    add_test(NAME xpcs_fake_xplane
        COMMAND xpcs_fake_xplane "$<TARGET_FILE:xpcs_plugin>" "${CMAKE_CURRENT_BINARY_DIR}/fake-xplane")
endif()

# Converts every X-Camera file the plugin would list into profile files (no X-Plane needed).
add_executable(xpcs_xcamera_convert ../../tools/xcamera_convert.cpp xp_storage.cpp)
target_link_libraries(xpcs_xcamera_convert PRIVATE fscs_profiles fscs_json)
target_include_directories(xpcs_xcamera_convert PRIVATE .)
