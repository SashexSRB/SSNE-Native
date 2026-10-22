# Cross-compile Windows x64 binaries from Linux with llvm-mingw
# (https://github.com/mstorsjo/llvm-mingw). Usage:
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DLLVM_MINGW=/path/to/llvm-mingw
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT LLVM_MINGW)
  set(LLVM_MINGW $ENV{LLVM_MINGW})
endif()
set(CMAKE_C_COMPILER   ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++)
set(CMAKE_RC_COMPILER  ${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
# Ship self-contained executables (no libc++/unwind DLLs next to them).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
