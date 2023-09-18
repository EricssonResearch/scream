# source files

#set(CMAKE_VERBOSE_MAKEFILE ON)

set(BUILD_SHARED_LIBS  on)

SET(HEADERS
../ScreamTx.h
../RtpQueue.h
)

SET(SRCS
../RtpQueue.cpp
../ScreamTx.cpp
../ScreamV1Tx.cpp
../ScreamV2Tx.cpp
../ScreamV2TxStream.cpp
screamtxbw_plugin_wrapper.cpp
screamtx_plugin_wrapper.cpp
)

set(CMAKE_BUILD_TYPE Debug)

if(BUILD_SHARED_LIBS)
  set(CMAKE_CXX_FLAGS                " -I.. ${CMAKE_CXX_FLAGS_INIT} ")
  add_library(scream SHARED ${SRCS})

else()

  set(CMAKE_CXX_FLAGS                " -I .. ${CMAKE_CXX_FLAGS_INIT} -fPIC")
  add_library(scream STATIC ${SRCS})
endif()


INCLUDE_DIRECTORIES(
${screamIncludes}
)
