cmake_minimum_required(VERSION 2.6)

PROJECT( scream )

message("Source Dir:" ${scream_SOURCE_DIR})

SET(EXECUTABLE_OUTPUT_PATH ${scream_SOURCE_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${scream_SOURCE_DIR}/lib)
SET(RUNTIME_OUTPUT_DIRECTORY ${scream_SOURCE_DIR}/bin)

SET(scream_BIN ${scream_SOURCE_DIR}/bin)

message("scream_SOURCE_DIR directories:" ${scream_SOURCE_DIR})

IF(UNIX)
add_definitions(-std=c++0x)
ENDIF(UNIX)

IF(WIN32)
IF(MSVC12)
message("Detected MSVC12 compiler")
set(MSVC_VER VC12)
ELSEIF(MSVC11)
message("Detected MSVC11 compiler")
set(MSVC_VER VC11)
ELSEIF(MSVC10)
message("Detected MSVC10 compiler")
set(MSVC_VER VC10)
ELSEIF(MSVC14)
message("Detected MSVC14 compiler")
set(MSVC_VER VC14)
ELSE(MSVC12)
message("WARNING: Unknown/unsupported MSVC version")
ENDIF(MSVC12)
ENDIF(WIN32)

if( CMAKE_SIZEOF_VOID_P EQUAL 8 )
# 64bit
  message("Detected 64-bit build - compiling with -fPIC")
  SET(CMAKE_CXX_FLAGS "-fPIC")
else( CMAKE_SIZEOF_VOID_P EQUAL 8 )
# 32 bit
message("Detected 32-bit build")
endif( CMAKE_SIZEOF_VOID_P EQUAL 8 )

SET(screamIncludes
${scream_SOURCE_DIR}
${scream_SOURCE_DIR}/code
)

message("screamIncludes directories:" ${screamIncludes})

# lib directories
IF(WIN32)
SET(screamLink
${scream_SOURCE_DIR}/../lib
)
ELSEIF(UNIX)
SET(screamLink
${scream_SOURCE_DIR}/../lib
/usr/local/lib
/usr/lib
)
ENDIF(WIN32)

SET(LibDir
${scream_SOURCE_DIR}/../lib
)

message("LibDir directories:" ${LibDir})

# Include directories
INCLUDE_DIRECTORIES(
${scream_SOURCE_DIR}/../include
)

ADD_SUBDIRECTORY( code) 
