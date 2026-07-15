##-----------------------------------------------------------------------------
##  Welcome to Streamripper
##-----------------------------------------------------------------------------
## cmake_minimum_required must precede project(); the old 2.6.0 floor is no
## longer accepted by CMake 4.x.  3.5 works with both modern (macOS) and
## older (Alpine) toolchains.
CMAKE_MINIMUM_REQUIRED (VERSION 3.5)

PROJECT (STREAMRIPPER)

## Tell CMake to be quiet
CMAKE_POLICY (SET CMP0003 NEW)

## Default to release
IF (NOT CMAKE_BUILD_TYPE)
  SET (CMAKE_BUILD_TYPE Release)
ENDIF (NOT CMAKE_BUILD_TYPE)

##-----------------------------------------------------------------------------
##  Include files
##-----------------------------------------------------------------------------
INCLUDE (CheckFunctionExists)
INCLUDE (CheckLibraryExists)
INCLUDE (FindPkgConfig)
INCLUDE (FindOpenGL)  # Why?

# http://www.cmake.org/pipermail/cmake/2008-December/025886.html
INCLUDE (InstallRequiredSystemLibraries)


##-----------------------------------------------------------------------------
##  Find libraries
##-----------------------------------------------------------------------------
IF (UNIX)
  SET (MATH_LIB -lm)
  ## Several source files gate their POSIX code (pthreads, arpa/inet.h, the
  ## abort pipe) on __UNIX__.  The historical autotools build defined it; the
  ## CMake build must too, or that code is silently dropped.
  ADD_DEFINITIONS (-D__UNIX__=1)
ELSE (UNIX)
  SET (MATH_LIB)
ENDIF (UNIX)

##-----------------------------------------------------------------------------
##  Self-contained (fully static) build
##-----------------------------------------------------------------------------
## Produces a binary with no shared-library dependencies.  Best on musl
## (Alpine); see Dockerfile.alpine.
OPTION (STREAMRIPPER_STATIC "Link a self-contained binary (static third-party libs)" OFF)
IF (STREAMRIPPER_STATIC)
  ## Prefer static archives when resolving libraries (both platforms).
  SET (CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  IF (APPLE)
    ## macOS has no static libSystem, so a fully static binary is impossible.
    ## "Self-contained" here means: link all third-party (Homebrew) libs
    ## statically via their full .a paths, leaving only the always-present
    ## macOS system libraries/frameworks dynamic.  Apple's ld has no
    ## -Bstatic/-Bdynamic or -static, so the per-library .a paths are resolved
    ## below (see the glib and FindXxx handling).
  ELSE (APPLE)
    ## Drop CMake's default -rdynamic, which would force a dynamic interpreter.
    SET (CMAKE_SHARED_LIBRARY_LINK_C_FLAGS "")
    SET (CMAKE_SHARED_LIBRARY_LINK_CXX_FLAGS "")
    ## Keep bare -l flags on the static side of the link line (no -Wl,-Bdynamic
    ## wrapping around glib/pcre2/etc.).
    SET (CMAKE_LINK_SEARCH_START_STATIC ON)
    SET (CMAKE_LINK_SEARCH_END_STATIC ON)
    SET (CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc")
  ENDIF (APPLE)
ENDIF (STREAMRIPPER_STATIC)

IF (UNIX)
  PKG_CHECK_MODULES (GLIB2 REQUIRED glib-2.0>=2.16)
  ## For a self-contained (static) build we need glib's full private
  ## dependency list (pcre2, etc.), which pkg-config only exposes via
  ## the _STATIC_ variables.
  IF (STREAMRIPPER_STATIC AND GLIB2_STATIC_LIBRARIES)
    IF (APPLE)
      ## Apple ld can't be told "link this -l statically", so resolve each
      ## glib dependency to its full .a path (CMAKE_FIND_LIBRARY_SUFFIXES is
      ## already ".a").  Anything without a static archive (e.g. system libm)
      ## falls back to a plain -l.  glib on macOS also pulls in a few system
      ## frameworks and libiconv.
      SET (_glib_static_libs "")
      FOREACH (_l ${GLIB2_STATIC_LIBRARIES})
        FIND_LIBRARY (GLIBDEP_${_l} NAMES ${_l}
          PATHS ${GLIB2_STATIC_LIBRARY_DIRS}
                /opt/homebrew/lib /usr/local/lib
                /opt/homebrew/opt/gettext/lib
                /opt/homebrew/opt/pcre2/lib
                /opt/homebrew/opt/glib/lib)
        IF (GLIBDEP_${_l})
          LIST (APPEND _glib_static_libs ${GLIBDEP_${_l}})
        ELSE (GLIBDEP_${_l})
          LIST (APPEND _glib_static_libs "-l${_l}")
        ENDIF (GLIBDEP_${_l})
      ENDFOREACH (_l)
      SET (GLIB2_LIBRARIES ${_glib_static_libs}
        "-liconv"
        "-framework CoreFoundation"
        "-framework Foundation"
        "-framework AppKit"
        "-framework Carbon")
    ELSE (APPLE)
      SET (GLIB2_LIBRARIES ${GLIB2_STATIC_LIBRARIES})
    ENDIF (APPLE)
  ENDIF (STREAMRIPPER_STATIC AND GLIB2_STATIC_LIBRARIES)
ENDIF (UNIX)

SET (CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
FIND_PACKAGE (IBERTY)
FIND_PACKAGE (MAD)
FIND_PACKAGE (OGG)
FIND_PACKAGE (VORBIS)

##-----------------------------------------------------------------------------
##  OpenSSL / TLS support (enables https:// streams)
##-----------------------------------------------------------------------------
OPTION (WITH_SSL "Build with TLS/https support (OpenSSL)" ON)
IF (WITH_SSL)
  ## Prefer the static archives when doing a self-contained build.
  IF (STREAMRIPPER_STATIC)
    SET (OPENSSL_USE_STATIC_LIBS TRUE)
  ENDIF (STREAMRIPPER_STATIC)
  FIND_PACKAGE (OpenSSL)
  IF (OPENSSL_FOUND)
    MESSAGE ("OpenSSL found: ${OPENSSL_VERSION} (${OPENSSL_LIBRARIES})")
    ADD_DEFINITIONS (-DHAVE_OPENSSL=1)
    INCLUDE_DIRECTORIES (BEFORE ${OPENSSL_INCLUDE_DIR})
    SET (SR_SSL_LIBS ${OPENSSL_LIBRARIES})
  ELSE (OPENSSL_FOUND)
    MESSAGE (WARNING "OpenSSL not found; https:// streams will not be supported")
    SET (SR_SSL_LIBS)
  ENDIF (OPENSSL_FOUND)
ELSE (WITH_SSL)
  SET (SR_SSL_LIBS)
ENDIF (WITH_SSL)

IF (WIN32)
  SET (MAD_LIBRARIES "${CMAKE_SOURCE_DIR}/win32/libmad-0.15.1b/lib/libmad.lib")
  SET (MAD_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/win32/libmad-0.15.1b/include")
ENDIF (WIN32)

INCLUDE (${CMAKE_SOURCE_DIR}/cmake/SR_FindSockets.cmake)

FIND_LIBRARY (SEMAPHORE_LIBRARIES NAMES sem rt)

##-----------------------------------------------------------------------------
##  Include directories
##-----------------------------------------------------------------------------
INCLUDE_DIRECTORIES (BEFORE ${GLIB2_INCLUDE_DIRS})
INCLUDE_DIRECTORIES (BEFORE ${CMAKE_CURRENT_BINARY_DIR})
INCLUDE_DIRECTORIES (BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/lib)

## glib is passed to the linker as bare -l flags via pkg-config; make sure its
## library directory is on the link path (matters where it isn't a system
## default, e.g. Homebrew's /opt/homebrew/lib on macOS).
IF (UNIX AND GLIB2_LIBRARY_DIRS)
  LINK_DIRECTORIES (${GLIB2_LIBRARY_DIRS})
ENDIF (UNIX AND GLIB2_LIBRARY_DIRS)

## Codec headers may live outside the default search path (e.g. Homebrew's
## /opt/homebrew/include on macOS), so add them explicitly when found.
IF (NOT WIN32)
  IF (MAD_INCLUDE_DIR)
    INCLUDE_DIRECTORIES (BEFORE ${MAD_INCLUDE_DIR})
  ENDIF (MAD_INCLUDE_DIR)
  IF (OGG_FOUND)
    INCLUDE_DIRECTORIES (BEFORE ${OGG_INCLUDE_DIR})
  ENDIF (OGG_FOUND)
  IF (VORBIS_FOUND)
    INCLUDE_DIRECTORIES (BEFORE ${VORBIS_INCLUDE_DIR})
  ENDIF (VORBIS_FOUND)
ENDIF (NOT WIN32)

IF (WIN32)
  INCLUDE_DIRECTORIES (BEFORE 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/glib-2.16.6-1/include/glib-2.0")
  INCLUDE_DIRECTORIES (BEFORE 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/glib-2.16.6-1/lib/glib-2.0/include")
  INCLUDE_DIRECTORIES (BEFORE "${MAD_INCLUDE_DIR}")
  INCLUDE_DIRECTORIES (BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/win32/iconv")
  INCLUDE_DIRECTORIES (BEFORE 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/libogg-1.1.3")
  INCLUDE_DIRECTORIES (BEFORE 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/libvorbis-1.1.2")
  INCLUDE_DIRECTORIES (BEFORE 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/zlib-1.2.3/include")
ENDIF (WIN32)

##-----------------------------------------------------------------------------
##  Configure include file
##-----------------------------------------------------------------------------
CONFIGURE_FILE (${CMAKE_CURRENT_SOURCE_DIR}/sr_config.h.cmake 
  ${CMAKE_CURRENT_BINARY_DIR}/sr_config.h)

##-----------------------------------------------------------------------------
##  Copy dlls
##-----------------------------------------------------------------------------
IF (WIN32)
  EXECUTE_PROCESS (COMMAND ${CMAKE_COMMAND} "-E" "copy" 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/glib-2.16.6-1/bin/libglib-2.0-0.dll"
    "${CMAKE_CURRENT_BINARY_DIR}")
  EXECUTE_PROCESS (COMMAND ${CMAKE_COMMAND} "-E" "copy" 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/mingw/libiconv-2.dll"
    "${CMAKE_CURRENT_BINARY_DIR}")
  EXECUTE_PROCESS (COMMAND ${CMAKE_COMMAND} "-E" "copy" 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/mingw/libintl-8.dll"
    "${CMAKE_CURRENT_BINARY_DIR}")
  EXECUTE_PROCESS (COMMAND ${CMAKE_COMMAND} "-E" "copy" 
    "${CMAKE_CURRENT_SOURCE_DIR}/win32/zlib-1.2.3/zlib1.dll"
    "${CMAKE_CURRENT_BINARY_DIR}")
ENDIF (WIN32)

##-----------------------------------------------------------------------------
##  Build subdirectories
##-----------------------------------------------------------------------------
ADD_SUBDIRECTORY (lib)

##-----------------------------------------------------------------------------
##  Source files
##-----------------------------------------------------------------------------
SET(STREAMRIPPER_SRC
	console/streamripper.c
	)

SET(WSTREAMRIPPER_SRC
	winamp_plugin/debug_box.c
	winamp_plugin/debug_box.h
	winamp_plugin/dock.c
	winamp_plugin/dock.h
	winamp_plugin/gen.h
	winamp_plugin/ioapi.c
	winamp_plugin/ioapi.h
	winamp_plugin/ipc_pe.h
	winamp_plugin/options.c
	winamp_plugin/options.h
#	winamp_plugin/plugin_dll_main.c
	winamp_plugin/plugin_main.c
	winamp_plugin/plugin_main.h
	winamp_plugin/registry.c
	winamp_plugin/registry.h
	winamp_plugin/render.c
	winamp_plugin/render.h
	winamp_plugin/render_2.c
	winamp_plugin/render_2.h
	winamp_plugin/resource.h
	winamp_plugin/unzip.c
	winamp_plugin/unzip.h
	winamp_plugin/wa_ipc.h
	winamp_plugin/wa_msgids.h
#	winamp_plugin/winamp_dll.c
#	winamp_plugin/winamp_dll.h
	winamp_plugin/winamp_exe.c
	winamp_plugin/winamp_exe.h
	winamp_plugin/wstreamripper.h
	winamp_plugin/Script.rc
	)

##-----------------------------------------------------------------------------
##  Targets
##-----------------------------------------------------------------------------
IF (WIN32)
  LINK_DIRECTORIES(
#	"${CMAKE_CURRENT_SOURCE_DIR}/libmad-0.15.1b/msvc++/Release"
	"${CMAKE_CURRENT_SOURCE_DIR}/win32/iconv"
	"${CMAKE_CURRENT_SOURCE_DIR}/tre-0.7.2/win32/Release"
#	"${CMAKE_CURRENT_SOURCE_DIR}/win32/glib-2.12.12/lib"
#	"${CMAKE_CURRENT_SOURCE_DIR}/win32/glib-2.16.3/lib"
#	"c:/gcs6/projects/sripper_1x/glib-gcs/no-unicows"
	"${CMAKE_CURRENT_SOURCE_DIR}/win32/glib-2.16.6-1/lib"
#	"c:/gcs6/pkg/libunicows/libunicows-1.1.2-msvc6"
#	"c:/gcs6/pkg/libunicows/libunicows-1.1.1-mingw"
	"${CMAKE_CURRENT_SOURCE_DIR}/win32/libogg-1.1.3"
	"${CMAKE_CURRENT_SOURCE_DIR}/win32/libvorbis-1.1.2"
	"${CMAKE_CURRENT_SOURCE_DIR}/win32/zlib-1.2.3/lib"
	)
ENDIF (WIN32)

MESSAGE ("MAD_LIBRARIES = ${MAD_LIBRARIES}")
SET (STREAMRIPPER_LIBS ${MAD_LIBRARIES} ${GLIB2_LIBRARIES} ${MATH_LIB})

IF (WIN32)
  SET (STREAMRIPPER_LIBS 
    ${STREAMRIPPER_LIBS}
    iconv unicows glib-2.0 unicows ws2_32 ogg vorbis zdll)
ENDIF (WIN32)

IF (SEMAPHORE_LIBRARIES)
  SET (STREAMRIPPER_LIBS ${STREAMRIPPER_LIBS} ${SEMAPHORE_LIBRARIES})
ENDIF (SEMAPHORE_LIBRARIES)

IF (IBERTY_FOUND)
  SET (STREAMRIPPER_LIBS ${STREAMRIPPER_LIBS} ${IBERTY_LIBRARIES})
ENDIF (IBERTY_FOUND)

IF (OGG_FOUND AND VORBIS_FOUND)
  SET (STREAMRIPPER_LIBS ${STREAMRIPPER_LIBS} ${OGG_LIBRARIES}
    ${VORBIS_LIBRARIES})
ENDIF (OGG_FOUND AND VORBIS_FOUND)

IF (SR_SSL_LIBS)
  SET (STREAMRIPPER_LIBS ${STREAMRIPPER_LIBS} ${SR_SSL_LIBS})
ENDIF (SR_SSL_LIBS)

SET (STREAMRIPPER_LIBS ${STREAMRIPPER_LIBS} ${SR_EXTRA_LIBS} ${MATH_LIB})

##-----------------------------------------------------------------------------
##  Linking
##-----------------------------------------------------------------------------
## chkstk.o may be required for mingw
IF (WIN32 AND (MSYS OR MINGW))
  SET (STREAMRIPPER_SRC ${STREAMRIPPER_SRC} 
  		       ${CMAKE_CURRENT_SOURCE_DIR}/win32/chkstk.o)
ENDIF (WIN32 AND (MSYS OR MINGW))

#ADD_LIBRARY (streamripper1 ${STREAMRIPPER_LIB_SRC})
ADD_EXECUTABLE (streamripper ${STREAMRIPPER_SRC})
TARGET_LINK_LIBRARIES (streamripper ${MY_MATH_LIB} 
  streamripper1 ${STREAMRIPPER_LIBS})

IF (WIN32)
    ADD_EXECUTABLE (wstreamripper WIN32 ${WSTREAMRIPPER_SRC})
    TARGET_LINK_LIBRARIES (wstreamripper ${MY_MATH_LIB} 
      streamripper1 ${STREAMRIPPER_LIBS}
      comctl32.lib
      )
ENDIF (WIN32)

IF (MSVC)
  ADD_EXECUTABLE (glib_iconv_test lib/glib_iconv_test.c)
  TARGET_LINK_LIBRARIES (glib_iconv_test ${STREAMRIPPER_LIBS})

IF (0)
  ADD_EXECUTABLE (glib_win98_test lib/glib_win98_test.c)
  TARGET_LINK_LIBRARIES (glib_win98_test ${STREAMRIPPER_LIBS})
ENDIF (0)
ENDIF (MSVC)

##-----------------------------------------------------------------------------
##  Install
##-----------------------------------------------------------------------------
INSTALL(TARGETS streamripper DESTINATION bin)
IF (WIN32)
  INSTALL(TARGETS wstreamripper DESTINATION bin)
ENDIF (WIN32)
