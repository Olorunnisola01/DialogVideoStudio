cmake_minimum_required(VERSION 3.21)
project(DialogVideoStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /W3)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Network Concurrent)

# --- vendored ONNX Runtime (DirectML build, 1.20.1) -------------------------
set(ORT_ROOT ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
add_library(onnxruntime SHARED IMPORTED)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${ORT_ROOT}/lib/onnxruntime.dll
    IMPORTED_IMPLIB   ${ORT_ROOT}/lib/onnxruntime.lib
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_ROOT}/include)

# --- dvs_core: everything except the widget UI ------------------------------
# Depends on Qt Core/Gui (QImage/QPainter) but not on Qt Widgets, so the whole
# pipeline stays drivable from the headless dvs_cli target.
add_library(dvs_core STATIC
    src/core/GpuDevice.cpp
    src/core/SrtParser.cpp
    src/core/Segmenter.cpp
    src/core/Fbank.cpp
    src/core/AudioDecoder.cpp
    src/core/Diarizer.cpp
    src/core/Translator.cpp
    src/core/TranslationService.cpp
    src/core/SecretStore.cpp
    src/core/Project.cpp
    src/core/Paths.cpp
    src/render/FrameRenderer.cpp
    src/render/VideoExporter.cpp
    src/render/VideoStitcher.cpp
)
target_include_directories(dvs_core PUBLIC src)
target_link_libraries(dvs_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network onnxruntime dxgi crypt32)

# --- GUI application --------------------------------------------------------
qt_add_executable(DialogVideoStudio WIN32
    src/main.cpp
    src/ui/MainWindow.cpp
    src/ui/HighlightDialog.cpp
    src/ui/OverlayPanel.cpp
    src/ui/PreviewCanvas.cpp
    src/ui/SegmentTableModel.cpp
    src/ui/SpeakerStylePanel.cpp
    src/ui/StitchDialog.cpp
    src/ui/StyleEditor.cpp
    src/ui/TimelineWidget.cpp
    src/ui/TranslationPanel.cpp
)
target_link_libraries(DialogVideoStudio PRIVATE
    dvs_core Qt6::Widgets Qt6::Multimedia Qt6::Concurrent)

# --- headless pipeline driver (build/verify without the UI) -----------------
add_executable(dvs_cli tests/dvs_cli.cpp)
target_link_libraries(dvs_cli PRIVATE dvs_core Qt6::Gui)

# --- runtime payload next to the executables --------------------------------
function(copy_runtime_payload target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ORT_ROOT}/lib/onnxruntime.dll $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${ORT_ROOT}/lib/DirectML.dll $<TARGET_FILE_DIR:${target}>
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/models $<TARGET_FILE_DIR:${target}>/models
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/tools $<TARGET_FILE_DIR:${target}>/tools
    )
endfunction()

copy_runtime_payload(DialogVideoStudio)
copy_runtime_payload(dvs_cli)

find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${Qt6_DIR}/../../../bin")
if(WINDEPLOYQT_EXECUTABLE)
    foreach(t DialogVideoStudio dvs_cli)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND ${WINDEPLOYQT_EXECUTABLE} --no-translations --no-opengl-sw $<TARGET_FILE:${t}>
        )
    endforeach()
    # windeployqt only ships the "windows" platform plugin. Add "offscreen" so
    # the CLI can render frames on a machine with no desktop session.
    get_filename_component(_qt_bin "${WINDEPLOYQT_EXECUTABLE}" DIRECTORY)
    foreach(t DialogVideoStudio dvs_cli)
        add_custom_command(TARGET ${t} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_qt_bin}/../plugins/platforms/qoffscreen.dll"
                "$<TARGET_FILE_DIR:${t}>/platforms/qoffscreen.dll"
        )
    endforeach()
else()
    message(WARNING "windeployqt not found - the built exe will need Qt's bin/ on PATH")
endif()

# --- install / package ------------------------------------------------------
install(TARGETS DialogVideoStudio RUNTIME DESTINATION .)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/models/ DESTINATION models)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/tools/ DESTINATION tools)
install(FILES ${ORT_ROOT}/lib/onnxruntime.dll ${ORT_ROOT}/lib/DirectML.dll DESTINATION .)
# windeployqt has already dropped the Qt runtime and its plugin folders next to
# the exe; sweep those up. Packaging always targets Release.
install(DIRECTORY ${CMAKE_BINARY_DIR}/Release/ DESTINATION .
        FILES_MATCHING PATTERN "*.dll"
        PATTERN "models" EXCLUDE PATTERN "tools" EXCLUDE)

# `cmake --build build --config Release --target dist` rebuilds the app and
# refreshes dist/ and the ZIP in one step. Packaging separately is easy to
# forget, and a stale dist/ looks exactly like features having been removed.
add_custom_target(dist
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --config $<CONFIG>
            --prefix ${CMAKE_SOURCE_DIR}/dist
    COMMAND ${CMAKE_CPACK_COMMAND} -C $<CONFIG> -G ZIP
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS DialogVideoStudio
    COMMENT "Refreshing dist/ and the distributable ZIP from the current build"
    VERBATIM)

set(CPACK_PACKAGE_NAME "DialogVideoStudio")
set(CPACK_PACKAGE_VENDOR "Adeleke Olorunnisola")
set(CPACK_PACKAGE_FILE_NAME "DialogVideoStudio-${PROJECT_VERSION}-win64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "DialogVideoStudio")
set(CPACK_GENERATOR "ZIP;NSIS")
set(CPACK_NSIS_PACKAGE_NAME "Dialog Video Studio")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
include(CPack)
