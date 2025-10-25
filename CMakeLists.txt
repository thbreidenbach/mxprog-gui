cmake_minimum_required(VERSION 3.21)
project(mxprog_qt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt Automatisierung
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets SerialPort)

add_executable(mxprog_qt
    main.cpp
    MainWindow.h MainWindow.cpp
    BankWidget.h BankWidget.cpp
)

target_link_libraries(mxprog_qt PRIVATE
    Qt6::Widgets
    Qt6::SerialPort
)

# macOS: App-Bundle erzeugen (Finder-freundlich) + Symlink auf das innere Binary
set_target_properties(mxprog_qt PROPERTIES MACOSX_BUNDLE TRUE)

if(APPLE)
  add_custom_command(TARGET mxprog_qt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
      "$<TARGET_BUNDLE_DIR:mxprog_qt>/Contents/MacOS/mxprog_qt"
      "${CMAKE_CURRENT_BINARY_DIR}/mxprog_qt"
    COMMENT "Creating convenience symlink build/mxprog_qt -> .app/Contents/MacOS/mxprog_qt"
  )
endif()

# Optional: make install
install(TARGETS mxprog_qt
  BUNDLE DESTINATION .
  RUNTIME DESTINATION bin
)
