cmake_minimum_required(VERSION 3.16)

project(shujianffuhe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    hiddevice.cpp
    hiddevice.h
    protocol.h
    dashboard.h
)

qt_add_executable(shujianffuhe WIN32 MANUAL_FINALIZATION ${PROJECT_SOURCES})

target_link_libraries(shujianffuhe PRIVATE
    Qt6::Widgets
    hid
    hidparse
    setupapi
)

set_target_properties(shujianffuhe PROPERTIES
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS shujianffuhe
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_finalize_executable(shujianffuhe)
