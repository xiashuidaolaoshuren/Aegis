find_package(GTest CONFIG REQUIRED)

add_executable(aegis_tests smoke_test.cpp money_ids_test.cpp iso8583_message_test.cpp iso8583_codec_test.cpp iso8583_roundtrip_test.cpp ledger_posting_test.cpp ledger_genesis_test.cpp ledger_lifecycle_test.cpp)
target_link_libraries(aegis_tests PRIVATE aegis GTest::gtest_main)
target_compile_definitions(aegis_tests PRIVATE "CORPUS_DIR=\"${CMAKE_SOURCE_DIR}/fuzz/corpus\"" "FIXTURES_DIR=\"${CMAKE_SOURCE_DIR}/fixtures\"")

include(GoogleTest)
gtest_discover_tests(aegis_tests)
