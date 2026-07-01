cmake_minimum_required(VERSION 3.22)
project(Wlameshot VERSION 1.0.0 LANGUAGES CXX)

# --- App identity (single source of truth) --------------------------------
# APP_ID    : binary/executable name, org id, desktop/icon basename
# APP_NAME  : human-readable display name (window/app name, tooltips)
set(APP_ID "wlameshot")
set(APP_NAME "Wlameshot")
set(APP_DESCRIPTION "Powerful screenshot tool for Wayland")
# --------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui)

qt_standard_project_setup(REQUIRES 6.5.0)

add_executable(${APP_ID}
    src/main.cpp
    src/capturewidget.cpp
    src/screenshotgrabber.cpp
)

target_compile_definitions(${APP_ID} PRIVATE
    APP_ID="${APP_ID}"
    APP_NAME="${APP_NAME}"
    APP_DESCRIPTION="${APP_DESCRIPTION}"
    APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(${APP_ID} PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Gui
)

set_target_properties(${APP_ID} PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER org.${APP_ID}
    MACOSX_RPATH TRUE
)

install(TARGETS ${APP_ID} RUNTIME DESTINATION bin)
