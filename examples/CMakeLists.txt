cmake_minimum_required (VERSION 3.9)

set(TARGET libvxcli)

include_directories("${CMAKE_SOURCE_DIR}/src")

add_executable(${TARGET} cli.c)

target_link_libraries(${TARGET} PRIVATE ${FFMPEG_LIBS})
target_link_libraries(${TARGET} PRIVATE Griffeye.VideoProcessor.Native)
target_link_libraries(${TARGET} PRIVATE m)