# XP Camera Studio: the X-Plane 12 plugin (doc: X-Plane plan). Built only with -DFSCS_BUILD_XPLANE=ON, so the MSFS
# build and its tests are unchanged. Targets X-Plane 12.4.4 and newer (SDK 4.4.0, XPLM440): the browser windows that
# will host the shared ui/ need it.

set(FSCS_XPSDK_DIR "${PROJECT_SOURCE_DIR}/third_party/XPSDK" CACHE PATH "X-Plane SDK root (CHeaders, Libraries)")
if(NOT EXISTS "${FSCS_XPSDK_DIR}/CHeaders/XPLM/XPLMDefs.h")
    message(FATAL_ERROR "X-Plane SDK not found at ${FSCS_XPSDK_DIR}")
endif()

add_library(xpcs_plugin MODULE plugin_main.cpp)
target_include_directories(xpcs_plugin SYSTEM PRIVATE "${FSCS_XPSDK_DIR}/CHeaders/XPLM")
# Every API level up to 4.4.0 (the macros are not cumulative).
target_compile_definitions(xpcs_plugin PRIVATE XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM400=1 XPLM410=1
                                               XPLM411=1 XPLM420=1 XPLM430=1 XPLM440=1)

# X-Plane loads <plugin folder>/<platform>/<name>.xpl.
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
