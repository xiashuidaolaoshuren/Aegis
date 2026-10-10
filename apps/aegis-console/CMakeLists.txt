find_package(Qt6 REQUIRED COMPONENTS Widgets)
if(NOT Qt6_FOUND)
    message(FATAL_ERROR
        "Qt6 Widgets not found. Use preset windows-msvc-console (vcpkg feature console) "
        "or set QT_INSTALL_DIR to a Qt 6 Widgets prefix before configuring.")
endif()

add_executable(aegis-console WIN32 main.cpp)
target_link_libraries(aegis-console PRIVATE aegis Qt6::Widgets)
