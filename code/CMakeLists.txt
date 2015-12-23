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

SET(SRC_2
${SRCS}
scream_02.cpp
)

SET(SRC_3
${SRCS}
scream_03.cpp
)



INCLUDE_DIRECTORIES(
${screamIncludes}
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_01 ${SRC_1} ${HEADERS})
ADD_EXECUTABLE(scream_02 ${SRC_2} ${HEADERS})
ADD_EXECUTABLE(scream_03 ${SRC_3} ${HEADERS})

TARGET_LINK_LIBRARIES (
scream_01
${screamLibs}
)

TARGET_LINK_LIBRARIES (
scream_02
${screamLibs}
)

TARGET_LINK_LIBRARIES (
scream_03
${screamLibs}
)

