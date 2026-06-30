# source files

SET(HEADERS
ScreamRx.h
ScreamTx.h
RtpQueue.h
)

SET(HEADERS_SIM
ScreamRx.h
ScreamTx.h
RtpQueue.h
NetQueue.h
OooQueue.h
VideoEnc.h
)

SET(SRC_SENDER
ScreamTx.cpp
ScreamV2Tx.cpp
ScreamV2TxStream.cpp
RtpQueue.cpp
scream_sender.cpp
)

SET(SRC_RECEIVER
ScreamRx.cpp
scream_receiver.cpp
)

SET(SCREAM_SIMULATOR
ScreamTx.cpp
ScreamV2Tx.cpp
ScreamV2TxStream.cpp
ScreamRx.cpp
RtpQueue.cpp
NetQueue.cpp
OooQueue.cpp
VideoEnc.cpp
scream_simulator.cpp
)

set(CMAKE_BUILD_TYPE Debug)

INCLUDE_DIRECTORIES(
${screamIncludes}
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_bw_test_tx ${SRC_SENDER} ${HEADERS})
ADD_EXECUTABLE(scream_bw_test_rx ${SRC_RECEIVER} ${HEADERS})
ADD_EXECUTABLE(scream_sim ${SCREAM_SIMULATOR} ${HEADERS_SIM} )

TARGET_LINK_LIBRARIES (
scream_bw_test_tx
${screamLibs} pthread
)

TARGET_LINK_LIBRARIES (
scream_bw_test_rx
${screamLibs} pthread
)

TARGET_LINK_LIBRARIES (
scream_sim
${screamLibs} pthread
)

target_compile_definitions(scream_sim PRIVATE IGNORE_PACKET)
