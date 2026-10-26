include_guard(GLOBAL)
include(FetchContent)
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a)
FetchContent_MakeAvailable(enet)
target_include_directories(enet PUBLIC "${enet_SOURCE_DIR}/include")
if(WIN32)
    target_link_libraries(enet PUBLIC winmm ws2_32)
endif()
