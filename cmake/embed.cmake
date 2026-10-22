# Converts a binary file to a C header: cmake -DIN=file -DOUT=header -DNAME=symbol -P embed.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," arr "${hex}")
math(EXPR bytes "${n} / 2")
file(WRITE "${OUT}" "// Generated from ${IN}\n#pragma once\nstatic const unsigned char ${NAME}[${bytes}] = {${arr}};\n")
