cmake_minimum_required(VERSION 3.16)

find_package(Qt6 REQUIRED COMPONENTS Core DBus Qml Quick)
find_package(ECM REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

set(KDE_INSTALL_USE_QT_SYS_PATHS ON CACHE BOOL "Install QML modules to the Qt prefix layout")
include(KDEInstallDirs6)
include(KDECMakeSettings)
include(ECMQmlModule)

find_package(KF6Package REQUIRED)
find_package(Plasma REQUIRED)

set(CMAKE_AUTOMOC ON)

# QML plugin: the only code that talks to btd700d, no HID library involved
add_library(btd700dbusplugin SHARED)
ecm_add_qml_module(btd700dbusplugin
    URI org.btd700ctl.dongle
    GENERATE_PLUGIN_SOURCE
)
target_sources(btd700dbusplugin PRIVATE dbusclient/dongleclient.cpp dbusclient/dongleclient.h)
target_include_directories(btd700dbusplugin PRIVATE dbusclient)
target_link_libraries(btd700dbusplugin PRIVATE Qt6::DBus Qt6::Qml Qt6::Quick)
ecm_finalize_qml_module(btd700dbusplugin DESTINATION ${KDE_INSTALL_QMLDIR})
install(TARGETS btd700dbusplugin LIBRARY DESTINATION ${KDE_INSTALL_QMLDIR}/org/btd700ctl/dongle)
set_target_properties(btd700dbuspluginplugin PROPERTIES INSTALL_RPATH "$ORIGIN")

plasma_install_package(package org.btd700ctl.dongle)

add_subdirectory(tests)
