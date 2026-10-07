add_executable(test_dongleclient
    test_dongleclient.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../dbusclient/dongleclient.cpp
)
target_include_directories(test_dongleclient PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../dbusclient)
target_link_libraries(test_dongleclient PRIVATE Qt6::Core Qt6::DBus Qt6::Qml)
target_compile_definitions(test_dongleclient PRIVATE BTD700D_PATH="$<TARGET_FILE:btd700d>")

find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME dongleclient COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:test_dongleclient>)
endif()
