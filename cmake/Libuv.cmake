find_package(PkgConfig QUIET)

if(PkgConfig_FOUND)
	pkg_check_modules(LIBUV REQUIRED libuv)
else()
	find_package(Libuv CONFIG REQUIRED)
endif()

if(NOT LIBUV_LIBRARIES)
	message(WARNING "LIBUV_LIBRARIES is empty. Attempting to use the system environment variable LIBUV_LIBRARIES.")
	set(LIBUV_LIBRARIES $ENV{LIBUV_LIBRARIES}/uv.lib)
	if(LIBUV_LIBRARIES)
		message(STATUS "Using LIBUV_LIBRARIES from environment: ${LIBUV_LIBRARIES}")
	else()
		message(FATAL_ERROR "LIBUV_LIBRARIES is not set, and no system environment variable was found.")
	endif()
else()
	message(STATUS "LIBUV_LIBRARIES is set to: ${LIBUV_LIBRARIES}")
endif()

if(NOT LIBUV_INCLUDE_DIRS)
	message(WARNING "LIBUV_INCLUDE_DIRS is empty. Attempting to use the system environment variable LIBUV_INCLUDE_DIRS.")
	set(LIBUV_INCLUDE_DIRS $ENV{LIBUV_INCLUDE_DIRS})
	if(LIBUV_INCLUDE_DIRS)
		message(STATUS "Using LIBUV_INCLUDE_DIRS from environment: ${LIBUV_INCLUDE_DIRS}")
	else()
		message(FATAL_ERROR "LIBUV_INCLUDE_DIRS is not set, and no system environment variable was found.")
	endif()
else()
	message(STATUS "LIBUV_INCLUDE_DIRS is set to: ${LIBUV_INCLUDE_DIRS}")
endif()