#
# Loads the Intel VPL library for hardware video acceleration
#
include_guard(GLOBAL)

if(NOT SUNSHINE_ENABLE_VPL)
    message(STATUS "Intel VPL support disabled")
    return()
endif()

if(NOT WIN32)
    message(STATUS "Intel VPL is only supported on Windows")
    set(SUNSHINE_ENABLE_VPL OFF CACHE BOOL "Enable Intel VPL support for hardware video acceleration." FORCE)
    return()
endif()

message(STATUS "Configuring Intel VPL support")

# Set VPL build options to match Sunshine's requirements
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build VPL as static library" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Don't build VPL tests" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "Don't build VPL examples" FORCE)
set(INSTALL_DEV OFF CACHE BOOL "Don't install VPL dev files" FORCE)
set(INSTALL_LIB OFF CACHE BOOL "Don't install VPL lib files" FORCE)
set(INSTALL_EXAMPLES OFF CACHE BOOL "Don't install VPL examples" FORCE)

# Add VPL as subdirectory
add_subdirectory("${CMAKE_SOURCE_DIR}/third-party/libvpl" EXCLUDE_FROM_ALL)

# Set VPL variables after subdirectory is processed
if(TARGET VPL)
    set(VPL_LIBRARIES VPL)
    set(VPL_INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/third-party/libvpl/api")
    message(STATUS "Intel VPL dispatcher library: VPL")
    message(STATUS "Intel VPL include dirs: ${VPL_INCLUDE_DIRS}")
else()
    message(WARNING "VPL target not found after adding subdirectory")
    set(SUNSHINE_ENABLE_VPL OFF CACHE BOOL "Enable Intel VPL support for hardware video acceleration." FORCE)
endif()