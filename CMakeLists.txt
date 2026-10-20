cmake_minimum_required(VERSION 3.25)
project(
    Lexongine
    VERSION 1.0.0
    LANGUAGES CXX)

option(LEXONGINE_XML_FRONTEND "Build XML configuration, native CLI and persistence" ON)
option(LEXONGINE_QAEDALUS_BRIDGE "Build integration tests against an external Qaedalus checkout"
       OFF)
set(LEXONGINE_QAEDALUS_SOURCE_DIR
    ""
    CACHE PATH "External Qaedalus source checkout (required with bridge ON)")
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

enable_testing()
add_subdirectory(integration/lexongine)
