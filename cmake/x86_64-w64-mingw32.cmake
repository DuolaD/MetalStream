set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The DLLs must link the Universal CRT. Homebrew's mingw-w64 defaults to it; on distros where
# x86_64-w64-mingw32 means msvcrt (Fedora, Debian), set MINGW_PREFIX=x86_64-w64-mingw32ucrt.
if(DEFINED ENV{MINGW_PREFIX})
    set(TOOLCHAIN_PREFIX $ENV{MINGW_PREFIX})
else()
    set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
endif()

find_program(CMAKE_C_COMPILER NAMES ${TOOLCHAIN_PREFIX}-gcc)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++)
find_program(CMAKE_RC_COMPILER NAMES ${TOOLCHAIN_PREFIX}-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
