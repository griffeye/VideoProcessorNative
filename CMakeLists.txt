cmake_minimum_required (VERSION 3.9)

set(FFMPEG_NUGET_NAME "ffmpeg-lgpl")
set(FFMPEG_NUGET_VERSION "6.1.0.2")

# Enable Hot Reload for MSVC compilers if supported.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

if(NOT WIN32)
    set(CMAKE_C_COMPILER gcc CACHE STRING "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER g++ CACHE STRING "C++ compiler" FORCE)
    set_property(TARGET ${PROJECT_NAME} PROPERTY C_STANDARD 23)
endif(NOT WIN32)

# Set up project information
file(READ "VERSION" PROJECT_FILE_VERSION)
project(Griffeye.VideoProcessor.Native VERSION ${PROJECT_FILE_VERSION} DESCRIPTION "Video frame extraction library using ffmpeg")

add_library(${PROJECT_NAME} SHARED
    src/libvx.c
    src/filter.c
    src/filtergraph.c
    src/util.c
)

# Set version, not required but good practice. Also symlink the major version to the full version.
set_target_properties(${PROJECT_NAME} PROPERTIES VERSION ${PROJECT_VERSION})
set_target_properties(${PROJECT_NAME} PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})

# Find and link to ffmpeg
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES VS_USER_PROPS "${CMAKE_CURRENT_SOURCE_DIR}/Griffeye.VideoProcessor.Native.deps.props")

    # Mandatory Nuget dependencies
    set_property(TARGET ${PROJECT_NAME}
        PROPERTY CMAKE_VS_NUGET_PACKAGE_RESTORE "ON"
    )
    set_property(TARGET ${PROJECT_NAME}
        PROPERTY VS_PACKAGE_REFERENCES "${FFMPEG_NUGET_NAME}_${FFMPEG_NUGET_VERSION}"
    )
else()
    target_include_directories(${PROJECT_NAME} PUBLIC
          "${CMAKE_CURRENT_SOURCE_DIR}/Packages/${FFMPEG_NUGET_NAME}.${FFMPEG_NUGET_VERSION}/build/native/Unix/include/x64"
    )

    # Assumes FFMpeg NuGet package have been explicitly/externally restored to ${CMAKE_CURRENT_SOURCE_DIR}/Packages
    set(FFMPEG_LIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/Packages/${FFMPEG_NUGET_NAME}.${FFMPEG_NUGET_VERSION}/build/native/Unix/lib/debug/x64")
    include(CMakePrintHelpers)
    cmake_print_variables(FFMPEG_LIB_PATH)

    find_library(AVUTIL_LIBRARY NAMES avutil PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(AVCODEC_LIBRARY NAMES avcodec PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(AVFORMAT_LIBRARY NAMES avformat PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(AVDEVICE_LIBRARY NAMES avdevice PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(SWSCALE_LIBRARY NAMES swscale PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(AVFILTER_LIBRARY NAMES avfilter PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(SWRESAMPLE_LIBRARY NAMES swresample PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(VPX_LIBRARY NAMES vpx PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    find_library(PKGCONF_LIBRARY NAMES pkgconf PATHS ${FFMPEG_LIB_PATH} REQUIRED NO_DEFAULT_PATH NO_CACHE)
    
    set(FFMPEG_LIBS ${AVDEVICE_LIBRARY} ${AVFILTER_LIBRARY} ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY} ${AVUTIL_LIBRARY} ${VPX_LIBRARY} ${PKGCONF_LIBRARY})
    cmake_print_variables(FFMPEG_LIBS)

    set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-Bsymbolic")

    target_link_libraries(${PROJECT_NAME} PUBLIC ${FFMPEG_LIBS})
endif()

# Public API
set_target_properties(${PROJECT_NAME} PROPERTIES PUBLIC_HEADER include/libvx.h)

# Include sub-projects.
add_subdirectory("src")
    
# Install library
install(TARGETS ${PROJECT_NAME} DESTINATION lib/${PROJECT_NAME})