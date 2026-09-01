cmake_minimum_required(VERSION 3.16)
project(CloudStore VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBEVENT REQUIRED libevent)
pkg_check_modules(LIBEVENT_PTHREADS libevent_pthreads)
if(NOT LIBEVENT_PTHREADS_LIBRARIES)
    find_library(LIBEVENT_PTHREADS_LIBRARIES event_pthreads)
endif()
pkg_check_modules(JSONCPP REQUIRED jsoncpp)
pkg_check_modules(HIREDIS REQUIRED hiredis)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_library(MYSQLCLIENT_LIB mysqlclient mariadb)
if(NOT MYSQLCLIENT_LIB)
    message(FATAL_ERROR "libmysqlclient / libmariadb not found")
endif()
find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LIBEVENT_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ${HIREDIS_INCLUDE_DIRS}
    ${MYSQL_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
)

set(COMMON_SOURCES
    src/config.cpp
    src/logger.cpp
    src/protocol.cpp
    src/utils.cpp
    src/crypto_util.cpp
    src/json_util.cpp
)

add_library(cloud_common STATIC ${COMMON_SOURCES})
target_link_libraries(cloud_common
    PUBLIC
        ${JSONCPP_LIBRARIES}
        OpenSSL::Crypto
        Threads::Threads
)

set(SERVER_SOURCES
    src/main.cpp
    src/server.cpp
    src/connection.cpp
    src/mysql_pool.cpp
    src/mysql_stmt.cpp
    src/redis_store.cpp
    src/auth.cpp
    src/path_guard.cpp
    src/file_manager.cpp
    src/transfer.cpp
)

add_executable(cloud_server ${SERVER_SOURCES})
target_link_libraries(cloud_server
    PRIVATE
        cloud_common
        ${LIBEVENT_LIBRARIES}
        ${LIBEVENT_PTHREADS_LIBRARIES}
        ${HIREDIS_LIBRARIES}
        ${MYSQLCLIENT_LIB}
        OpenSSL::Crypto
        Threads::Threads
)

set(CLIENT_SOURCES
    client/main.cpp
    client/client.cpp
)

add_executable(cloud_client ${CLIENT_SOURCES})
target_include_directories(cloud_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/client)
target_link_libraries(cloud_client
    PRIVATE
        cloud_common
        Threads::Threads
)

option(BUILD_GUI "Build web GUI (cloud_gui)" ON)
if(BUILD_GUI)
    set(GUI_SOURCES
        client/gui_main.cpp
        client/client.cpp
    )
    add_executable(cloud_gui ${GUI_SOURCES})
    target_include_directories(cloud_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/client
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    )
    target_compile_definitions(cloud_gui PRIVATE CPPHTTPLIB_USE_NON_BLOCKING_GETADDRINFO=0)
    target_link_libraries(cloud_gui
        PRIVATE
            cloud_common
            Threads::Threads
    )
    install(DIRECTORY client/web DESTINATION share/cloudstore)
endif()

install(TARGETS cloud_server cloud_client RUNTIME DESTINATION bin)
if(BUILD_GUI)
    install(TARGETS cloud_gui RUNTIME DESTINATION bin)
endif()
install(FILES conf/server.conf DESTINATION etc/cloudstore)
