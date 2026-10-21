add_executable(kquickshare main.cpp receiver.cpp receiver.h senddialog.cpp senddialog.h)
target_link_libraries(kquickshare PRIVATE kqscore Qt6::Widgets Qt6::DBus KF6::Notifications KF6::StatusNotifierItem KF6::Prison)
install(TARGETS kquickshare RUNTIME DESTINATION bin)
install(FILES ../../packaging/org.kde.kquickshare.desktop DESTINATION share/applications)
install(FILES ../../packaging/kquickshare.notifyrc DESTINATION share/knotifications6)

install(PROGRAMS ../../packaging/kquickshare-send.desktop DESTINATION share/kio/servicemenus)
configure_file(../../packaging/org.kde.kquickshare.service.in org.kde.kquickshare.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kquickshare.service DESTINATION share/dbus-1/services)

configure_file(../../packaging/org.kde.kquickshare.systemd.in org.kde.kquickshare.systemd @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.kquickshare.systemd DESTINATION share/systemd/user RENAME org.kde.kquickshare.service)

install(DIRECTORY ../../integration/ DESTINATION share/kquickshare/integration USE_SOURCE_PERMISSIONS)

install(FILES icons/org.kde.kquickshare.svg icons/org.kde.kquickshare-symbolic.svg
    DESTINATION share/icons/hicolor/scalable/apps)
install(FILES icons/org.kde.kquickshare-symbolic.svg
    DESTINATION share/icons/hicolor/22x22/apps)

install(FILES ../../README.md ../../README.ru.md DESTINATION share/doc/kquickshare)
