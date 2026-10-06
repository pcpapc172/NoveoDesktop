# Keep the pinned vendor checkout clean; compile the explicit media opt-in copy.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(noveo_webview_source "${CMAKE_CURRENT_SOURCE_DIR}/lib_webview")
set(noveo_webview_patched "${CMAKE_CURRENT_BINARY_DIR}/noveo_webview")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_noveo_webview.py")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/prepare_noveo_webview.py"
        "${noveo_webview_source}" "${noveo_webview_patched}"
    COMMAND_ERROR_IS_FATAL ANY
)
add_subdirectory("${noveo_webview_patched}" "${CMAKE_CURRENT_BINARY_DIR}/lib_webview")
