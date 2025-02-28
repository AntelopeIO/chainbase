# Defines ChainBase library target.
cmake_minimum_required( VERSION 3.12 )
project( ChainBase )

#list( APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/CMakeModules" )

set(CMAKE_EXPORT_COMPILE_COMMANDS "ON")

SET( Boost_USE_STATIC_LIBS ON CACHE STRING "ON or OFF" )

include( GNUInstallDirs )

IF( WIN32 )
  SET(BOOST_ROOT $ENV{BOOST_ROOT})
  set(Boost_USE_MULTITHREADED ON)
  set(BOOST_ALL_DYN_LINK OFF) # force dynamic linking for all libraries
ENDIF(WIN32)

if(NOT TARGET Boost::unit_test_framework)
   if(POLICY CMP0167)
      cmake_policy(SET CMP0167 NEW)
   endif()
   FIND_PACKAGE(Boost 1.67 REQUIRED COMPONENTS system unit_test_framework)
endif()

SET(PLATFORM_LIBRARIES)

if(CMAKE_CXX_STANDARD EQUAL 98)
   message(FATAL_ERROR "chainbase requires c++20 or newer")
elseif(NOT CMAKE_CXX_STANDARD)
   set(CMAKE_CXX_STANDARD 20)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if( APPLE )
  # Apple Specific Options Here
  message( STATUS "Configuring ChainBase on OS X" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wno-conversion" )
else( APPLE )
  # Linux Specific Options Here
  message( STATUS "Configuring ChainBase on Linux" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall" )
  if ( FULL_STATIC_BUILD )
    set( CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")
  endif ( FULL_STATIC_BUILD )
  LIST( APPEND PLATFORM_LIBRARIES pthread )
endif( APPLE )

if( "${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU" )
  set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-builtin-memcmp" )
endif()

if( "${CMAKE_GENERATOR}" STREQUAL "Ninja" )
  if( "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" )
      set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcolor-diagnostics" )
  endif()
endif()

# based on http://www.delorie.com/gnu/docs/gdb/gdb_70.html
# uncomment this line to tell GDB about macros (slows compile times)
# set( CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -gdwarf-2 -g3" )

set(ENABLE_COVERAGE_TESTING FALSE CACHE BOOL "Build ChainBase for code coverage analysis")

if(ENABLE_COVERAGE_TESTING)
    SET(CMAKE_CXX_FLAGS "--coverage ${CMAKE_CXX_FLAGS}")
endif()


file(GLOB HEADERS "include/chainbase/*.hpp")
add_library( chainbase src/chainbase.cpp src/pinnable_mapped_file.cpp ${HEADERS} )
target_link_libraries( chainbase PUBLIC ${PLATFORM_LIBRARIES} Boost::system )

if(TARGET Boost::asio)
  target_link_libraries( chainbase PUBLIC
                         Boost::headers Boost::interprocess Boost::chrono Boost::multi_index Boost::lexical_cast Boost::asio )
endif()

target_include_directories( chainbase PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"  )

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 8 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries( chainbase PUBLIC stdc++fs )
  endif()
endif()

if(WIN32)
   target_link_libraries( chainbase PUBLIC ws2_32 mswsock )
endif()

# for BSD we should avoid any pthread calls such as pthread_mutex_lock 
# in boost/interprocess
if(${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
  target_compile_definitions(chainbase PUBLIC BOOST_INTERPROCESS_FORCE_GENERIC_EMULATION)
endif()

enable_testing()
add_subdirectory( test )
add_subdirectory( benchmark )

if(CHAINBASE_INSTALL_COMPONENT)
   set(INSTALL_COMPONENT_ARGS COMPONENT ${CHAINBASE_INSTALL_COMPONENT} EXCLUDE_FROM_ALL)
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/chainbase DESTINATION ${CMAKE_INSTALL_FULL_INCLUDEDIR} ${INSTALL_COMPONENT_ARGS})

install(TARGETS chainbase
   LIBRARY DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} ${INSTALL_COMPONENT_ARGS}
   ARCHIVE DESTINATION ${CMAKE_INSTALL_FULL_LIBDIR} ${INSTALL_COMPONENT_ARGS})
