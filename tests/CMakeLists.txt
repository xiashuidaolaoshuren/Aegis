find_package(GTest CONFIG REQUIRED)

add_executable(aegis_tests smoke_test.cpp money_ids_test.cpp iso8583_message_test.cpp)
target_link_libraries(aegis_tests PRIVATE aegis GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(aegis_tests)
