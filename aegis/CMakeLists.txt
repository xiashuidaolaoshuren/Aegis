add_library(aegis STATIC placeholder.cpp ledger/ledger.cpp net/socket.cpp)
target_include_directories(aegis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(WIN32)
    target_link_libraries(aegis PUBLIC ws2_32)
endif()
