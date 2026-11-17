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

add_executable(test_gamesave Modules/test_gamesave.c)
add_dependencies(test_gamesave xgameruntime xgameruntime-idl)
target_include_directories(test_gamesave PRIVATE ${PROJECT_SOURCE_DIR}/include ${IDL_BINARY_DIR})
target_compile_definitions(test_gamesave PRIVATE __WINESRC__ COBJMACROS WIDL_C_INLINE_WRAPPERS)
target_link_options(test_gamesave PRIVATE -static -static-libgcc)
set_target_properties(test_gamesave PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
configure_file(Modules/MicrosoftGame.config "${CMAKE_BINARY_DIR}/bin/MicrosoftGame.config" COPYONLY)
if(CMAKE_CROSSCOMPILING)
    set_property(TARGET test_gamesave PROPERTY CROSSCOMPILING_EMULATOR "${WINE_EXECUTABLE}")
endif()
add_test(NAME gamesave_errors COMMAND test_gamesave)

add_executable(test_user_tokens Modules/test_user_tokens.c)
add_dependencies(test_user_tokens xgameruntime xgameruntime-idl)
target_include_directories(test_user_tokens PRIVATE ${PROJECT_SOURCE_DIR}/include ${IDL_BINARY_DIR})
target_compile_definitions(test_user_tokens PRIVATE __WINESRC__ COBJMACROS WIDL_C_INLINE_WRAPPERS)
target_link_options(test_user_tokens PRIVATE -static -static-libgcc)
set_target_properties(test_user_tokens PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME user_token_unicode COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Modules/run_user_token_tests.py"
    --wine "${WINE_EXECUTABLE}" --binary "$<TARGET_FILE:test_user_tokens>")
