# source files

SET(HEADERS
ScreamRx.h
ScreamTx.h
RtpQueue.h
)

SET(SRC_SENDER
ScreamTx.cpp
RtpQueue.cpp
scream_sender.cpp
)

SET(SRC_RECEIVER
ScreamRx.cpp
scream_receiver.cpp
)


INCLUDE_DIRECTORIES(
${screamIncludes}
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_bw_test_tx ${SRC_SENDER} ${HEADERS})
ADD_EXECUTABLE(scream_bw_test_rx ${SRC_RECEIVER} ${HEADERS})

TARGET_LINK_LIBRARIES (
scream_bw_test_tx
${screamLibs} pthread
)

TARGET_LINK_LIBRARIES (
scream_bw_test_rx
${screamLibs} pthread
)
