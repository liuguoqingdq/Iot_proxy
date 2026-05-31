cmake_minimum_required(VERSION 3.12)

project(myring LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(myring
  src/TcpConnection.cpp
  src/TcpServer.cpp
  src/Kcp/KcpManager.cpp
  src/Kcp/UdpDispatcher.cpp
  src/Kcp/KcpSession.cpp
  src/Proactor/LoopThreadPool.cpp
  src/Proactor/Proactor.cpp
  src/Proactor/ProactorThread.cpp
  src/objectpool/ObjectPool.cpp
  src/tcmalloc/CentralCache.cpp
  src/tcmalloc/MiniMalloc.cpp
  src/tcmalloc/PageHeap.cpp
  src/tcmalloc/ThreadCache.cpp
  src/tcmalloc/TransferCache.cpp
  thirdparty/ikcp/ikcp.c
)

add_library(myring_support ALIAS myring)

target_include_directories(myring
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/ikcp
)

target_link_libraries(myring
  PUBLIC
    uring
)
