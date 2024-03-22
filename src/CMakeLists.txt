# CMakeList.txt : CMake project for Griffeye.VideoProcessor.Native, include source and define
# project specific logic here.
#

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Griffeye.VideoProcessor.Native PROPERTY CXX_STANDARD 23)
endif()