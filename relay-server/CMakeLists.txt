cmake_minimum_required(VERSION 3.16)
project(p2pChatRelayServer CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)
find_package(LibJuice CONFIG REQUIRED)
find_path(ASIO_INCLUDE_DIR asio.hpp)
if(NOT ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "Standalone Asio headers are required to build p2pChatRelayServer")
endif()

add_executable(p2pChatRelayServer main.cpp RoomRelayServer.cpp ../RoomProtocol.cpp)
target_include_directories(p2pChatRelayServer PRIVATE ${ASIO_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_definitions(p2pChatRelayServer PRIVATE ASIO_STANDALONE)
target_link_libraries(p2pChatRelayServer PRIVATE Threads::Threads OpenSSL::SSL OpenSSL::Crypto LibJuice::LibJuice)

if(WIN32)
  target_compile_definitions(p2pChatRelayServer PRIVATE _WIN32_WINNT=0x0A00)
  target_link_libraries(p2pChatRelayServer PRIVATE ws2_32 mswsock)
endif()

if(MSVC)
  target_compile_options(p2pChatRelayServer PRIVATE /W4)
else()
  target_compile_options(p2pChatRelayServer PRIVATE -Wall -Wextra)
endif()
