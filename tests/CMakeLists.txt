add_executable(test_xgameruntime
        System/test_xthreading.cpp
        test_fixtures.cpp)

target_link_libraries(test_xgameruntime
        PRIVATE
        libxgameruntime
        GTest::gtest_main )

target_link_options(test_xgameruntime PRIVATE
        -static
        -static-libgcc
        -static-libstdc++ )

target_compile_features(test_xgameruntime
        PUBLIC
        cxx_std_23 )

include( GoogleTest )

target_include_directories(test_xgameruntime
        PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/external/googletest/googletest/include )

# For discovery purposes.
set_target_properties(test_xgameruntime PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" )

if( CMAKE_CROSSCOMPILING )
    message( STATUS "We are cross compiling tests" )
    find_program( WINE_EXECUTABLE wine )
    message( STATUS "Using wine: ${WINE_EXECUTABLE}" )
    if( NOT WINE_EXECUTABLE )
        message( FATAL_ERROR "Wine is required to run Windows tests on non-Windows hosts" )
    endif()
    set_property(
            TARGET test_xgameruntime
            PROPERTY CROSSCOMPILING_EMULATOR
            "${WINE_EXECUTABLE}" )
endif()

# We should not be pre-running tests during build.
gtest_discover_tests(
        test_xgameruntime
        DISCOVERY_MODE PRE_TEST )

# ------------------------------------------------------------
# C modules (src/Modules): smoke test through QueryApiImpl
# ------------------------------------------------------------
add_executable(test_modules
        Modules/test_modules.c )

add_dependencies(test_modules
        xgameruntime
        xgameruntime-idl )

target_include_directories(test_modules
        PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${IDL_BINARY_DIR} )

target_compile_definitions(test_modules
        PRIVATE
        __WINESRC__
        COBJMACROS
        WIDL_C_INLINE_WRAPPERS )

target_link_options(test_modules PRIVATE
        -static
        -static-libgcc )

set_target_properties(test_modules PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin" )

if( CMAKE_CROSSCOMPILING )
    set_property(
            TARGET test_modules
            PROPERTY CROSSCOMPILING_EMULATOR
            "${WINE_EXECUTABLE}" )
endif()

add_test(NAME modules_smoke COMMAND test_modules)
