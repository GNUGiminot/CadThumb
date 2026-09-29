cmake_minimum_required(VERSION 3.21)

project(CadThumb VERSION 1.0.3 LANGUAGES CXX RC)

option(CADTHUMB_WITH_STEP "Build STEP support (requires OpenCASCADE)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# РЎС‚Р°С‚РёС‡РµСЃРєРёР№ CRT: DLL РіСЂСѓР·РёС‚СЃСЏ РІ dllhost/explorer, EXE РЅРµ С‚СЂРµР±СѓРµС‚ VC Redist.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
                        CADTHUMB_VERSION="${PROJECT_VERSION}")
add_compile_options(/utf-8 /W3 /MP /permissive- /Zc:__cplusplus)

find_package(miniz CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
if(TARGET pugixml::static)
  set(PUGIXML_TARGET pugixml::static)
else()
  set(PUGIXML_TARGET pugixml::pugixml)
endif()

# ---------------------------------------------------------------- common
add_library(cadthumb_common STATIC
  src/common/Paths.cpp
  src/common/Settings.cpp
  src/common/Log.cpp
  src/common/Hash.cpp
  src/common/FileType.cpp
  src/common/ImageIO.cpp
  src/common/Zip.cpp
  src/common/ThreeMfPackage.cpp
  src/common/PipeClient.cpp
  src/common/Registration.cpp
  src/common/Cache.cpp
  src/common/ExplorerRestart.cpp
)
target_include_directories(cadthumb_common PUBLIC src)
target_link_libraries(cadthumb_common PUBLIC miniz::miniz ${PUGIXML_TARGET} windowscodecs shlwapi ole32 advapi32 shell32 uuid)

# ---------------------------------------------------------------- render
add_library(cadthumb_render STATIC
  src/render/Rasterizer.cpp
  src/render/StlLoader.cpp
  src/render/ThreeMfLoader.cpp
  src/render/RenderFile.cpp
)
target_link_libraries(cadthumb_render PUBLIC cadthumb_common ${PUGIXML_TARGET})

if(CADTHUMB_WITH_STEP)
  find_package(OpenCASCADE CONFIG REQUIRED)
  target_sources(cadthumb_render PRIVATE src/render/StepLoader.cpp)
  target_compile_definitions(cadthumb_render PUBLIC CADTHUMB_WITH_STEP=1)
  target_include_directories(cadthumb_render PRIVATE ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(cadthumb_render PRIVATE
    TKDESTEP TKXCAF TKRWMesh TKMesh TKBRep TKTopAlgo TKGeomAlgo TKGeomBase TKG3d TKG2d TKMath TKernel
    TKXSBase TKLCAF TKCAF TKCDF TKShHealing TKDE TKVCAF TKV3d TKService TKHLR TKBO TKPrim
    opengl32 user32 gdi32 advapi32 ws2_32)
endif()

# ---------------------------------------------------------------- shell extension DLL
add_library(CadThumbShell SHARED
  src/shellext/dllmain.cpp
  src/shellext/ThumbnailProvider.cpp
  src/shellext/CadThumbShell.def
)
target_link_libraries(CadThumbShell PRIVATE cadthumb_common)

# ---------------------------------------------------------------- host / worker / tray EXE
add_executable(CadThumb WIN32
  src/app/main.cpp
  src/app/JobManager.cpp
  src/app/PipeServer.cpp
  src/app/RenderRunner.cpp
  src/app/Prewarm.cpp
  src/app/Tray.cpp
  src/app/Console.cpp
  src/app/Diagnose.cpp
  src/viewer/Viewer.cpp
  src/viewer/ViewModel.cpp
  res/CadThumb.rc
)
target_link_libraries(CadThumb PRIVATE cadthumb_render comctl32 oleaut32 opengl32 glu32)
set_target_properties(CadThumb PROPERTIES LINK_FLAGS "/MANIFESTUAC:NO")

# ---------------------------------------------------------------- test tool
add_executable(thumbtest src/tools/thumbtest.cpp)
target_link_libraries(thumbtest PRIVATE cadthumb_common)

include(CTest)
if(BUILD_TESTING)
  add_executable(cadthumb_model_tests tests/model_tests.cpp src/viewer/ViewModel.cpp)
  target_link_libraries(cadthumb_model_tests PRIVATE cadthumb_render)
  target_compile_definitions(cadthumb_model_tests PRIVATE
    CADTHUMB_TEST_DIR=L"${CMAKE_CURRENT_BINARY_DIR}/model-tests")
  add_test(NAME model-formats COMMAND cadthumb_model_tests)
  add_executable(cadthumb_pipe_tests tests/pipe_tests.cpp src/app/PipeServer.cpp src/app/JobManager.cpp)
  target_include_directories(cadthumb_pipe_tests PRIVATE src)
  target_link_libraries(cadthumb_pipe_tests PRIVATE ole32 advapi32)
  target_compile_definitions(cadthumb_pipe_tests PRIVATE
    CADTHUMB_TEST_DIR=L"${CMAKE_CURRENT_BINARY_DIR}/model-tests")
  add_test(NAME pipe-protocol COMMAND cadthumb_pipe_tests)
  add_executable(cadthumb_installer_tests tests/installer_tests.cpp)
  target_link_libraries(cadthumb_installer_tests PRIVATE cadthumb_common comctl32)
  target_compile_definitions(cadthumb_installer_tests PRIVATE CADTHUMB_VERSION=L"${PROJECT_VERSION}")
  add_test(NAME installer-state COMMAND cadthumb_installer_tests)
endif()

# ---------------------------------------------------------------- single-file installer
# The freshly built CadThumb.exe + CadThumbShell.dll are copied to a fixed place and embedded as RCDATA.
set(PAYLOAD_DIR "${CMAKE_CURRENT_BINARY_DIR}/payload")
add_custom_command(
  OUTPUT "${PAYLOAD_DIR}/CadThumb.exe" "${PAYLOAD_DIR}/CadThumbShell.dll"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${PAYLOAD_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:CadThumb>" "${PAYLOAD_DIR}/CadThumb.exe"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:CadThumbShell>" "${PAYLOAD_DIR}/CadThumbShell.dll"
  DEPENDS CadThumb CadThumbShell "$<TARGET_FILE:CadThumb>" "$<TARGET_FILE:CadThumbShell>"
  VERBATIM)
add_custom_target(CadThumbPayload DEPENDS "${PAYLOAD_DIR}/CadThumb.exe" "${PAYLOAD_DIR}/CadThumbShell.dll")
configure_file(res/CadThumbSetupPayload.rc.in "${CMAKE_CURRENT_BINARY_DIR}/CadThumbSetupPayload.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/CadThumbSetupPayload.rc" PROPERTIES
  OBJECT_DEPENDS "${PAYLOAD_DIR}/CadThumb.exe;${PAYLOAD_DIR}/CadThumbShell.dll")

add_executable(CadThumbSetup WIN32
  src/installer/main.cpp
  res/CadThumbSetup.rc
  "${CMAKE_CURRENT_BINARY_DIR}/CadThumbSetupPayload.rc"
)
add_dependencies(CadThumbSetup CadThumbPayload)
target_link_libraries(CadThumbSetup PRIVATE cadthumb_common comctl32)
