# Minimal Pico SDK locator for this project.
if (DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif ()

if (NOT PICO_SDK_PATH)
    set(_USER_HOME $ENV{USERPROFILE})
    if (NOT _USER_HOME)
        set(_USER_HOME $ENV{HOME})
    endif ()
    if (EXISTS "${_USER_HOME}/.ds5-build/pico-sdk/external/pico_sdk_import.cmake")
        set(PICO_SDK_PATH "${_USER_HOME}/.ds5-build/pico-sdk" CACHE PATH "Path to the Raspberry Pi Pico SDK" FORCE)
    endif ()
endif ()

if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "Set PICO_SDK_PATH or install the Pico SDK cache at ~/.ds5-build/pico-sdk")
endif ()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
