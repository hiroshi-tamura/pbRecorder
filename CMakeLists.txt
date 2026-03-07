cmake_minimum_required(VERSION 3.24)
project(pbRecorder VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Suppress CMake policy warnings for old third-party libs
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "")

# Qt6
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Gui Multimedia)

# Windows SDK libraries
set(WIN_LIBS
    d3d11
    dxgi
    mf
    mfplat
    mfreadwrite
    mfuuid
    mfcore
    ole32
    oleaut32
    uuid
    wmcodecdspuuid
    strmiids
    dwmapi
    shcore
    propsys
)

# Third-party: libmatroska, libopus, libvorbis
# These will be built from source via FetchContent
include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# libebml
FetchContent_Declare(
    libebml
    GIT_REPOSITORY https://github.com/Matroska-Org/libebml.git
    GIT_TAG        release-1.4.5
)
FetchContent_MakeAvailable(libebml)

# Fix: ebml_export.h is in the binary dir but only added as PRIVATE include.
# Make it PUBLIC so libmatroska (and our code) can find it.
target_include_directories(ebml PUBLIC
    $<BUILD_INTERFACE:${libebml_BINARY_DIR}>
)

# Point libmatroska to the FetchContent-built libebml
set(EBML_DIR "${libebml_BINARY_DIR}" CACHE PATH "" FORCE)
# Create a minimal EBMLConfig.cmake so find_package(EBML) works
file(WRITE "${libebml_BINARY_DIR}/EBMLConfig.cmake" "
set(EBML_FOUND TRUE)
set(EBML_VERSION \"1.4.5\")
set(EBML_INCLUDE_DIRS \"${libebml_SOURCE_DIR}\" \"${libebml_BINARY_DIR}\")
set(EBML_LIBRARIES ebml)
if(NOT TARGET EBML::ebml)
    add_library(EBML::ebml ALIAS ebml)
endif()
")
file(WRITE "${libebml_BINARY_DIR}/EBMLConfigVersion.cmake" "
set(PACKAGE_VERSION \"1.4.5\")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT FALSE)
")

FetchContent_Declare(
    libmatroska
    GIT_REPOSITORY https://github.com/Matroska-Org/libmatroska.git
    GIT_TAG        release-1.7.1
)
FetchContent_MakeAvailable(libmatroska)

# Fix: matroska_export.h is generated in the binary dir, same issue as ebml
target_include_directories(matroska PUBLIC
    $<BUILD_INTERFACE:${libmatroska_BINARY_DIR}>
)

# ogg
FetchContent_Declare(
    ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG        v1.3.5
)
FetchContent_MakeAvailable(ogg)

# Point vorbis to FetchContent-built ogg
set(OGG_ROOT "${ogg_BINARY_DIR}" CACHE PATH "" FORCE)
set(OGG_INCLUDE_DIR "${ogg_SOURCE_DIR}/include;${ogg_BINARY_DIR}/include" CACHE PATH "" FORCE)
set(OGG_LIBRARY "ogg" CACHE STRING "" FORCE)
file(WRITE "${ogg_BINARY_DIR}/OggConfig.cmake" "
set(OGG_FOUND TRUE)
set(Ogg_FOUND TRUE)
set(OGG_VERSION \"1.3.5\")
set(OGG_INCLUDE_DIRS \"${ogg_SOURCE_DIR}/include\" \"${ogg_BINARY_DIR}/include\")
set(OGG_LIBRARIES ogg)
if(NOT TARGET Ogg::ogg)
    add_library(Ogg::ogg ALIAS ogg)
endif()
")
file(WRITE "${ogg_BINARY_DIR}/OggConfigVersion.cmake" "
set(PACKAGE_VERSION \"1.3.5\")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
set(PACKAGE_VERSION_EXACT FALSE)
")
set(Ogg_DIR "${ogg_BINARY_DIR}" CACHE PATH "" FORCE)

FetchContent_Declare(
    vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG        v1.3.7
)
FetchContent_MakeAvailable(vorbis)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG        v1.4
)
FetchContent_MakeAvailable(opus)

# ASIO SDK (user must place ASIO SDK in third_party/asiosdk)
set(ASIO_SDK_DIR "${CMAKE_SOURCE_DIR}/third_party/asiosdk")
if(EXISTS "${ASIO_SDK_DIR}/common/asio.h")
    set(ASIO_AVAILABLE ON)
    message(STATUS "ASIO SDK found at ${ASIO_SDK_DIR}")
else()
    set(ASIO_AVAILABLE OFF)
    message(STATUS "ASIO SDK not found - ASIO support disabled")
endif()

# Source files
set(CORE_SOURCES
    src/core/D3DManager.cpp
    src/core/RecordingSession.cpp
    src/core/MonitorEnumerator.cpp
    src/core/WindowEnumerator.cpp
)

set(CAPTURE_SOURCES
    src/capture/DxgiScreenCapture.cpp
    src/capture/WindowCapture.cpp
    src/capture/RegionCapture.cpp
)

set(AUDIO_SOURCES
    src/audio/WasapiCapture.cpp
    src/audio/AsioCapture.cpp
)

set(PIPELINE_SOURCES
    src/pipeline/SinkWriterPipeline.cpp
    src/pipeline/MkvPipeline.cpp
)

set(UI_SOURCES
    src/ui/MainWindow.cpp
    src/ui/SettingsDialog.cpp
    src/ui/RegionSelectorWidget.cpp
)

set(UI_FILES
    src/ui/MainWindow.ui
)

set(ALL_SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${CAPTURE_SOURCES}
    ${AUDIO_SOURCES}
    ${PIPELINE_SOURCES}
    ${UI_SOURCES}
)

# ASIO sources
if(ASIO_AVAILABLE)
    list(APPEND ALL_SOURCES src/audio/AsioCapture.cpp)
    list(APPEND ALL_SOURCES
        ${ASIO_SDK_DIR}/common/asio.cpp
        ${ASIO_SDK_DIR}/host/asiodrivers.cpp
        ${ASIO_SDK_DIR}/host/pc/asiolist.cpp
    )
endif()

add_executable(${PROJECT_NAME} WIN32 ${ALL_SOURCES} ${UI_FILES} resources/pbRecorder.rc)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/capture
    ${CMAKE_SOURCE_DIR}/src/audio
    ${CMAKE_SOURCE_DIR}/src/pipeline
    ${CMAKE_SOURCE_DIR}/src/ui
)

if(ASIO_AVAILABLE)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${ASIO_SDK_DIR}/common
        ${ASIO_SDK_DIR}/host
        ${ASIO_SDK_DIR}/host/pc
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE ASIO_AVAILABLE=1)
endif()

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Widgets
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
    ${WIN_LIBS}
    ebml
    matroska
    Ogg::ogg
    vorbis
    vorbisenc
    opus
)

# Copy Qt DLLs on install
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
qt_generate_deploy_app_script(
    TARGET ${PROJECT_NAME}
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
