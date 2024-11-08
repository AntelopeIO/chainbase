find_package(X11 REQUIRED)

file(GLOB UNIT_TESTS "*.cpp")
add_executable( chainbase_test ${UNIT_TESTS} )
target_include_directories( chainbase_test PRIVATE ${X11_INCLUDE_DIR} ../deps/glad/include ../deps/glm)
target_link_libraries( chainbase_test  chainbase Boost::unit_test_framework ${PLATFORM_SPECIFIC_LIBS} ${X11_LIBRARIES} glfw OpenGL::GL)

add_test(chainbase_test chainbase_test)
