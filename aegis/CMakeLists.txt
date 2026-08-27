add_library(aegis STATIC placeholder.cpp ledger/ledger.cpp)
target_include_directories(aegis PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
