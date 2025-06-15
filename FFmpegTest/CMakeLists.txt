# CMakeList.txt : CMake project for FFmpegTest, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.12)
# Add source to this project's executable.
add_executable (FFmpegTest "FFmpegTest.cpp" "FFmpegTest.h")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET FFmpegTest PROPERTY CXX_STANDARD 20)
endif()


set(FFMPEG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/ffmpeg/include")
set(FFMPEG_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/ffmpeg/lib")
set(SDL2_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/sdl2/include")
set(SDL2_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/sdl2/lib")

# Specify the include directories for FFmpeg libraries
include_directories(${FFMPEG_INCLUDE_DIR})
link_directories(${FFMPEG_LIBRARY_DIR})

#Specifiy the libraries for SDL
include_directories(${SDL2_INCLUDE_DIR})
link_directories(${SDL2_LIBRARY_DIR})

#Link the FFmpeg and SDL2 libraries to the executable
target_link_libraries(FFmpegTest PRIVATE
	"${FFMPEG_LIBRARY_DIR}/avcodec.lib"
	"${FFMPEG_LIBRARY_DIR}/avformat.lib"
	"${FFMPEG_LIBRARY_DIR}/avutil.lib"
	"${FFMPEG_LIBRARY_DIR}/swscale.lib"
	"${SDL2_LIBRARY_DIR}/SDL2.lib"
)

