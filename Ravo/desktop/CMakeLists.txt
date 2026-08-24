qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

add_library(ravo_desktop STATIC)

target_sources(ravo_desktop
  PRIVATE
    src/studio_presenter.cpp
    include/ravo/desktop/studio_presenter.h)

target_include_directories(ravo_desktop
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

target_link_libraries(ravo_desktop
  PUBLIC ravo_services ravo_adapters
  PRIVATE Qt6::Gui)

set_target_properties(ravo_desktop PROPERTIES AUTOMOC ON)
ravo_configure_target(ravo_desktop)

add_executable(ravo_studio src/main.cpp)
target_link_libraries(ravo_studio
  PRIVATE
    ravo_desktop
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Qt6::QuickLayouts)

set_target_properties(ravo_studio PROPERTIES AUTOMOC ON)
ravo_configure_target(ravo_studio)
ravo_deploy_qt_core(ravo_studio)

qt_add_qml_module(ravo_studio
  URI Ravo.Studio
  VERSION 1.0
  QML_FILES qml/Main.qml)
