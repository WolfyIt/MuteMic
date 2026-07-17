cmake_minimum_required(VERSION 3.15)
project(MuteMic LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MuteMic WIN32
    main.cpp
    audio.cpp
    tray.cpp
    icons.cpp
    autostart.cpp
)

target_compile_definitions(MuteMic PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(MuteMic PRIVATE ole32 shell32 user32 gdi32 advapi32)

if(MSVC)
    # Release chico: /O2 + runtime estático (/MT), sin RTTI innecesario.
    target_compile_options(MuteMic PRIVATE $<$<CONFIG:Release>:/O2>)
    set_property(TARGET MuteMic PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
