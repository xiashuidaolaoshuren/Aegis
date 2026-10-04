add_executable(aegisd aegisd/main.cpp)
target_link_libraries(aegisd PRIVATE aegis)

add_executable(aegis-load aegis-load/main.cpp)
target_link_libraries(aegis-load PRIVATE aegis)

if(AEGIS_BUILD_CONSOLE)
    add_subdirectory(aegis-console)
endif()
