project (mysql-replication-listener)
cmake_minimum_required (VERSION 2.6)

set(MySQL_BINLOG_VERSION_MAJOR "0")
set(MySQL_BINLOG_VERSION_MINOR "0.1")
set(MRL_VERSION "${MySQL_BINLOG_VERSION_MAJOR}.${MySQL_BINLOG_VERSION_MINOR}")

set(CMAKE_VERSION_STRING "${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}")

# General settings
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)

include_directories(include)
link_directories(${PROJECT_BINARY_DIR}/lib)

# TODO: order: find asio -> allow to define asio directory -> use include/asio
# For ASIO headers
INCLUDE_DIRECTORIES(include/asio)

# --------- Find crypt ------------------------------------------
FIND_LIBRARY(LIB_CRYPTO crypto /opt/local/lib /opt/lib /usr/lib /usr/local/lib)

add_subdirectory(src)

include(InstallRequiredSystemLibraries)

# installation configuration
install(DIRECTORY include DESTINATION /usr FILES_MATCHING PATTERN "*.h")

