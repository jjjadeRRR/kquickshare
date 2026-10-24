set(CPACK_PACKAGE_NAME "kquickshare")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "KQuickShare contributors")
set(CPACK_PACKAGE_CONTACT "https://github.com/jjjadeRRR/kquickshare/issues")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Android Quick Share file and text transfers for KDE Plasma 6")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jjjadeRRR/kquickshare")
set(CPACK_GENERATOR "TGZ")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "jjjadeRRR <jjjadeRRR@users.noreply.github.com>")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6svg6, qt6-wayland")
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later AND GPL-2.0-or-later")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
# Fedora runtime plugins are loaded dynamically and missed by automatic ELF deps.
set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtsvg, qt6-qtwayland")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share /usr/share/applications /usr/share/icons /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps
    /usr/share/icons/hicolor/22x22 /usr/share/icons/hicolor/22x22/apps
    /usr/share/dbus-1 /usr/share/dbus-1/services /usr/lib /usr/lib/systemd /usr/lib/systemd/user)
include(CPack)
