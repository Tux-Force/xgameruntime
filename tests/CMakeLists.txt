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
# Own directory with its own MicrosoftGame.config and DLL copy, so the other tests keep running
# without a title config.
set_target_properties(test_gamesave PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/gamesave")
configure_file(Modules/MicrosoftGame.config "${CMAKE_BINARY_DIR}/bin/gamesave/MicrosoftGame.config" COPYONLY)
add_custom_command(TARGET test_gamesave POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:xgameruntime>" "$<TARGET_FILE_DIR:test_gamesave>/")
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
# Needs Python for the fake bridge; without it the test is skipped. On Windows the binary runs
# directly, when cross compiling through Wine.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME user_token_unicode COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Modules/run_user_token_tests.py"
        --wine "${WINE_EXECUTABLE}" --binary "$<TARGET_FILE:test_user_tokens>"
        --dll "$<TARGET_FILE:xgameruntime>")
endif()

add_executable(test_addons Modules/test_addons.c)
add_dependencies(test_addons xgameruntime xgameruntime-idl)
target_include_directories(test_addons PRIVATE ${PROJECT_SOURCE_DIR}/include ${IDL_BINARY_DIR})
target_compile_definitions(test_addons PRIVATE __WINESRC__ COBJMACROS WIDL_C_INLINE_WRAPPERS)
target_link_options(test_addons PRIVATE -static -static-libgcc)
set_target_properties(test_addons PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(Python3_FOUND)
    add_test(NAME addons_dlc COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/Modules/run_addon_tests.py"
        --wine "${WINE_EXECUTABLE}" --binary "$<TARGET_FILE:test_addons>"
        --dll "$<TARGET_FILE:xgameruntime>")
endif()
