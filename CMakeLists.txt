cmake_minimum_required(VERSION 3.16)
project(kquickshare VERSION 0.1.0 LANGUAGES CXX)

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
    src/core/nearbyserver.cpp
)
target_include_directories(kqscore PUBLIC src/core)
target_link_libraries(kqscore PUBLIC Qt6::Core Qt6::Network OpenSSL::Crypto)

add_executable(kquickshare-cli src/cli/main.cpp)
target_link_libraries(kquickshare-cli PRIVATE kqscore)

# --- GUI (optional, built by default when deps are found) ---
find_package(Qt6 COMPONENTS Quick QUIET)
find_package(KF6Kirigami QUIET)
find_package(KF6Notifications QUIET)
find_package(KF6StatusNotifierItem QUIET)
find_package(KF6I18n QUIET)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/CMakeLists.txt
    AND Qt6Quick_FOUND AND KF6Kirigami_FOUND AND KF6Notifications_FOUND
    AND KF6StatusNotifierItem_FOUND AND KF6I18n_FOUND)
    add_subdirectory(src/app)
else()
    message(STATUS "GUI not configured yet (or deps missing), building CLI only")
endif()

install(TARGETS kquickshare-cli RUNTIME DESTINATION bin)
