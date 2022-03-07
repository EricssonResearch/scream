# source files
SET(SRCS
../../../../code/ScreamRx.cpp
scream_receiver.cpp
)

SET(HEADERS
../../../../code/ScreamRx.h
)

SET(SRC_1
${SRCS}
scream_receiver.cpp
)

INCLUDE_DIRECTORIES(
${screamIncludes}
../../../../code/
)

LINK_DIRECTORIES(
${screamLink}
)

ADD_EXECUTABLE(scream_receiver ${SRC_1} ${HEADERS})


TARGET_LINK_LIBRARIES (
scream_receiver
${screamLibs}
)
