cmake_minimum_required(VERSION 3.16)
project(p2pChat CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(P2P_BUILD_APP   "Build the console application (needs FTXUI)" ON)
option(P2P_BUILD_TESTS "Build the tests" ON)

include(FetchContent)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
# ngtcp2's OpenSSL QUIC TLS backend requires OpenSSL's QUIC API (3.5+).
find_package(OpenSSL 3.5 REQUIRED COMPONENTS SSL Crypto)
find_package(ngtcp2 CONFIG REQUIRED)

# standalone asio
find_path(ASIO_INCLUDE_DIR asio.hpp)
if(NOT ASIO_INCLUDE_DIR)
  FetchContent_Declare(asio GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git GIT_TAG asio-1-30-2)
  FetchContent_GetProperties(asio)
  if(NOT asio_POPULATED)
    FetchContent_Populate(asio)
  endif()
  set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif()

# everything except the UI
add_library(p2pcore STATIC
  PeerId.cpp TextSanitizer.cpp Message.cpp CryptoIdentity.cpp Session.cpp PeerManager.cpp StorageManager.cpp)
target_include_directories(p2pcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${ASIO_INCLUDE_DIR})
target_compile_definitions(p2pcore PUBLIC ASIO_STANDALONE)
target_link_libraries(p2pcore PUBLIC Threads::Threads SQLite3::SQLite3 OpenSSL::SSL OpenSSL::Crypto
  ngtcp2::ngtcp2 ngtcp2::ngtcp2_crypto_ossl)
if(WIN32)
  target_compile_definitions(p2pcore PUBLIC _WIN32_WINNT=0x0A00)
  target_link_libraries(p2pcore PUBLIC ws2_32 mswsock Crypt32)
endif()
if(MSVC)
  target_compile_options(p2pcore PUBLIC /W4)
else()
  target_compile_options(p2pcore PUBLIC -Wall -Wextra)
endif()

if(P2P_BUILD_APP)
  if(FTXUI_SOURCE_DIR)
    add_subdirectory(${FTXUI_SOURCE_DIR} ftxui EXCLUDE_FROM_ALL)
  else()
    # Prefer the package declared in vcpkg.json; retain the pinned FetchContent
    # fallback for developers building without vcpkg.
    find_package(ftxui CONFIG QUIET)
    if(NOT ftxui_FOUND)
      FetchContent_Declare(ftxui GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git GIT_TAG v6.1.9)
      FetchContent_MakeAvailable(ftxui)
    endif()
  endif()
  add_executable(p2pChat main.cpp Interface.cpp ConsoleInterface.cpp)
  target_link_libraries(p2pChat PRIVATE p2pcore ftxui::screen ftxui::dom ftxui::component)
endif()

if(P2P_BUILD_TESTS)
  enable_testing()
  add_executable(p2p_tests tests.cpp)
  target_link_libraries(p2p_tests PRIVATE p2pcore)
  add_test(NAME p2p_tests COMMAND p2p_tests)
endif()
