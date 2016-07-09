# source files
SET(SRCS
NetQueue.cpp
RtpQueue.cpp
ScreamRx.cpp
ScreamTx.cpp
VideoEnc.cpp
)

SET(HEADERS
NetQueue.h
RtpQueue.h
ScreamRx.h
ScreamTx.h
VideoEnc.h
)

SET(SRC_1
${SRCS}
scream_01.cpp
)

INCLUDE_DIRECTORIES(
${screamIncludes}
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_01 ${SRC_1} ${HEADERS})

TARGET_LINK_LIBRARIES (
scream_01
${screamLibs}
)
