cmake_minimum_required(VERSION 3.16)
project(kquickshare VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(OpenSSL REQUIRED)

add_library(kqscore STATIC
    src/core/protobuf.cpp
    src/core/crypto.cpp
    src/core/endpointinfo.cpp
    src/core/frames.cpp
    src/core/mdnsresponder.cpp
    src/core/inboundconnection.cpp
    src/core/devicediscovery.cpp
    src/core/outboundconnection.cpp
    src/core/nearbyserver.cpp
)
target_include_directories(kqscore PUBLIC src/core)
target_link_libraries(kqscore PUBLIC Qt6::Core Qt6::Network OpenSSL::Crypto)

add_executable(kquickshare-cli src/cli/main.cpp)
target_link_libraries(kquickshare-cli PRIVATE kqscore)

option(BUILD_GUI "Build the native KDE application" ON)
if(BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)
    find_package(KF6Notifications REQUIRED)
    find_package(KF6StatusNotifierItem REQUIRED)
    find_package(KF6Prison REQUIRED)
    add_subdirectory(src/app)
endif()

install(TARGETS kquickshare-cli RUNTIME DESTINATION bin)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(core-tests tests/core_tests.cpp)
    target_link_libraries(core-tests PRIVATE kqscore Qt6::Test)
    add_test(NAME core-tests COMMAND core-tests)
endif()
if(BUILD_TESTING AND BUILD_GUI)
    add_executable(gui-tests tests/gui_tests.cpp src/app/receiver.cpp src/app/receiver.h src/app/senddialog.cpp src/app/senddialog.h)
    target_include_directories(gui-tests PRIVATE src/app)
    target_compile_definitions(gui-tests PRIVATE SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(gui-tests PRIVATE kqscore Qt6::Widgets Qt6::DBus Qt6::Test KF6::Notifications KF6::StatusNotifierItem KF6::Prison)
    find_program(DBUS_RUN_SESSION dbus-run-session)
    if(DBUS_RUN_SESSION)
        add_test(NAME gui-tests COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:gui-tests>)
        set_tests_properties(gui-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
    endif()
endif()
add_executable(kquickshare-send src/cli/send.cpp)
target_link_libraries(kquickshare-send PRIVATE kqscore)
if(BUILD_TESTING AND BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Quick Qml)
    find_package(KF6I18n REQUIRED)
    add_executable(clipboard-tests tests/clipboard_tests.cpp)
    get_target_property(QT_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    execute_process(COMMAND ${QT_QMAKE_EXECUTABLE} -query QT_INSTALL_QML OUTPUT_VARIABLE QT_QML_ROOT OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CLIPBOARD_MODULE_DIR "${CMAKE_CURRENT_BINARY_DIR}/clipboard-qml/org/kde/plasma/private/clipboard")
    file(MAKE_DIRECTORY "${CLIPBOARD_MODULE_DIR}")
    file(GLOB CLIPBOARD_QML_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/integration/qml/org/kde/plasma/private/clipboard/*.qml" "${CMAKE_CURRENT_SOURCE_DIR}/integration/qml/org/kde/plasma/private/clipboard/qmldir")
    foreach(source IN LISTS CLIPBOARD_QML_SOURCES)
        get_filename_component(name "${source}" NAME)
        configure_file("${source}" "${CLIPBOARD_MODULE_DIR}/${name}" COPYONLY)
    endforeach()
    file(CREATE_LINK "${QT_QML_ROOT}/org/kde/plasma/private/clipboard/libklipperplugin.so" "${CLIPBOARD_MODULE_DIR}/libklipperplugin.so" SYMBOLIC)
    target_compile_definitions(clipboard-tests PRIVATE SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" CLIPBOARD_QML_PATH="${CMAKE_CURRENT_BINARY_DIR}/clipboard-qml")
    target_link_libraries(clipboard-tests PRIVATE Qt6::Quick Qt6::Qml Qt6::Test Qt6::DBus KF6::I18nQml)
    if(DBUS_RUN_SESSION)
        add_test(NAME clipboard-tests COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:clipboard-tests>)
        set_tests_properties(clipboard-tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_FORCE_STDERR_LOGGING=1")
    endif()
endif()

install(FILES LICENSE NOTICE.md DESTINATION share/licenses/kquickshare)
