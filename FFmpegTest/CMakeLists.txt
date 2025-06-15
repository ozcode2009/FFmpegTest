# CMakeList.txt : CMake project for FFmpegTest, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.12)

# Add source to this project's executable.
add_executable (FFmpegTest "FFmpegTest.cpp" "FFmpegTest.h")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET FFmpegTest PROPERTY CXX_STANDARD 20)
endif()

# Set output directory for executables
set_target_properties(FFmpegTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)

# Platform-specific library handling
if(WIN32)
    # Windows: Use provided libraries
    set(FFMPEG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/ffmpeg/include")
    set(FFMPEG_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/ffmpeg/lib")
    set(SDL2_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/sdl2/include")
    set(SDL2_LIBRARY_DIR "${CMAKE_SOURCE_DIR}/FFmpegTest/sdl2/lib")
    
    # Specify the include directories
    include_directories(${FFMPEG_INCLUDE_DIR})
    link_directories(${FFMPEG_LIBRARY_DIR})
    include_directories(${SDL2_INCLUDE_DIR})
    link_directories(${SDL2_LIBRARY_DIR})
    
    # Link the libraries
    target_link_libraries(FFmpegTest PRIVATE
        "${FFMPEG_LIBRARY_DIR}/avcodec.lib"
        "${FFMPEG_LIBRARY_DIR}/avformat.lib"
        "${FFMPEG_LIBRARY_DIR}/avutil.lib"
        "${FFMPEG_LIBRARY_DIR}/swscale.lib"
        "${SDL2_LIBRARY_DIR}/SDL2.lib"
    )
    
else()
    # Linux/macOS: Use pkg-config
    find_package(PkgConfig REQUIRED)
    
    # Find FFmpeg libraries
    pkg_check_modules(FFMPEG REQUIRED 
        libavcodec 
        libavformat 
        libavutil 
        libswscale
    )
    
    # Find SDL2
    pkg_check_modules(SDL2 REQUIRED sdl2)
    
    # Add include directories
    target_include_directories(FFmpegTest PRIVATE 
        ${FFMPEG_INCLUDE_DIRS}
        ${SDL2_INCLUDE_DIRS}
    )
    
    # Add compile flags
    target_compile_options(FFmpegTest PRIVATE 
        ${FFMPEG_CFLAGS_OTHER}
        ${SDL2_CFLAGS_OTHER}
    )
    
    # Link libraries
    target_link_libraries(FFmpegTest PRIVATE 
        ${FFMPEG_LIBRARIES}
        ${SDL2_LIBRARIES}
    )
    
    # Add library directories
    target_link_directories(FFmpegTest PRIVATE 
        ${FFMPEG_LIBRARY_DIRS}
        ${SDL2_LIBRARY_DIRS}
    )
    
endif()
