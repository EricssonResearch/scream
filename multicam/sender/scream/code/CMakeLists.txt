# source files
SET(SRCS
RtpQueue.cpp
../../../../code/ScreamTx.cpp
scream_sender.cpp
)

SET(HEADERS
RtpQueue.h
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
