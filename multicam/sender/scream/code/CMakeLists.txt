# source files
SET(SRCS
../../../../code/RtpQueue.cpp
../../../../code/ScreamTx.cpp
../../../../code/ScreamV1Tx.cpp
../../../../code/ScreamV2Tx.cpp
../../../../code/ScreamV2TxStream.cpp
scream_sender.cpp
)

SET(HEADERS
../../../../code/RtpQueue.h
../../../../code/ScreamTx.h
)

SET(SRC_1
${SRCS}
scream_sender.cpp
)

INCLUDE_DIRECTORIES(
${screamIncludes}
../../../../code/
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_sender ${SRC_1} ${HEADERS})


TARGET_LINK_LIBRARIES (
scream_sender
${screamLibs}
)
