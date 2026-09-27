cmake_minimum_required(VERSION 3.16)
project(cadthumb-thumbnailer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PUGIXML REQUIRED IMPORTED_TARGET pugixml)
find_package(Threads REQUIRED)

# OpenCASCADE ships a CMake config on most distros (Debian/Ubuntu package: libocct-data-exchange-dev),
# but it only sets OpenCASCADE_INCLUDE_DIR / OpenCASCADE_LIBRARY_DIR, not per-module imported targets.
find_package(OpenCASCADE REQUIRED)

# Rootless CI/dev verification only (see linux/build/): overrides the paths above to point at a
# .deb-extracted sysroot instead of /usr, because the packaged config hardcodes absolute /usr paths.
# Real installs (via the distro's package manager) never set this and are unaffected.
if(DEFINED ENV{CADTHUMB_TEST_SYSROOT})
  set(OpenCASCADE_INCLUDE_DIR "$ENV{CADTHUMB_TEST_SYSROOT}/usr/include/opencascade")
  set(OpenCASCADE_LIBRARY_DIR "$ENV{CADTHUMB_TEST_SYSROOT}/usr/lib/x86_64-linux-gnu")
endif()

add_compile_definitions(CADTHUMB_VERSION="${PROJECT_VERSION}")
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_executable(cadthumb-thumbnailer
  src/main.cpp
  src/common/Paths.cpp
  src/common/FileType.cpp
  src/common/Settings.cpp
  src/common/ImageIO.cpp
  src/common/Zip.cpp
  src/common/ThreeMfPackage.cpp
  src/render/Rasterizer.cpp
  src/render/StlLoader.cpp
  src/render/ThreeMfLoader.cpp
  src/render/StepLoader.cpp
  src/render/RenderFile.cpp
  third_party/miniz/miniz.c
)

target_include_directories(cadthumb-thumbnailer PRIVATE
  src
  third_party/miniz
  third_party/stb
  ${OpenCASCADE_INCLUDE_DIR}
)
target_link_directories(cadthumb-thumbnailer PRIVATE ${OpenCASCADE_LIBRARY_DIR})

# stb_image/stb_image_write are single-header amalgamations; their implementation is compiled once
# from common/ImageIO.cpp (STB_IMAGE_IMPLEMENTATION / STB_IMAGE_WRITE_IMPLEMENTATION defined there).
set_source_files_properties(src/common/ImageIO.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")

# Direct dependencies only (OCCT_MODULE): each .so is linked against its own transitive deps by the
# distro package, so the linker resolves the rest at load time. Names follow the OCCT 7.6 module
# layout used by Debian/Ubuntu (older than the merged TKDE/TKDESTEP names of OCCT 7.7+/vcpkg).
target_link_libraries(cadthumb-thumbnailer PRIVATE
  PkgConfig::PUGIXML
  Threads::Threads
  TKXDESTEP    # STEPCAFControl_Reader (XDE-aware STEP import)
  TKSTEP       # STEPControl_Reader (plain fallback)
  TKXCAF       # XCAFDoc_*, XCAFApp_Application, XCAFPrs_DocumentExplorer
  TKRWMesh     # RWMesh_FaceIterator
  TKMesh       # BRepMesh_IncrementalMesh
  TKBRep TKTopAlgo TKGeomAlgo TKGeomBase TKG3d TKG2d TKMath TKernel
  TKXSBase TKLCAF TKCAF TKCDF TKShHealing
)
