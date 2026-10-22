find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(T ${CMAKE_CURRENT_BINARY_DIR})
set(GEN ${T}/recompiled)

# 1) synthetic discs  2) extract test.iso  3) recompile its main.dol
add_custom_command(
  OUTPUT ${T}/test.iso ${T}/wrong.iso ${T}/junk.bin ${T}/test.dol
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_test_iso.py ${T}
  DEPENDS make_test_iso.py)
add_custom_command(
  OUTPUT ${GEN}/funcs_000.cpp ${GEN}/func_table.cpp ${GEN}/recomp_funcs.h
  COMMAND $<TARGET_FILE:ssne-recomp> ${T}/test.dol ${GEN}
  DEPENDS ssne-recomp ${T}/test.dol)

add_executable(cpu_test cpu_test_main.cpp ${GEN}/funcs_000.cpp ${GEN}/func_table.cpp)
target_include_directories(cpu_test PRIVATE ${GEN})
target_link_libraries(cpu_test PRIVATE ssne-runtime)

add_test(NAME verify_accepts_game  COMMAND gctool verify ${T}/test.iso ${CMAKE_SOURCE_DIR}/data/known_dumps.txt)
add_test(NAME verify_rejects_other COMMAND gctool verify ${T}/wrong.iso)
set_tests_properties(verify_rejects_other PROPERTIES WILL_FAIL TRUE)
add_test(NAME verify_rejects_junk  COMMAND gctool verify ${T}/junk.bin)
set_tests_properties(verify_rejects_junk PROPERTIES WILL_FAIL TRUE)
add_test(NAME extract COMMAND gctool extract ${T}/test.iso ${T}/extracted)
add_test(NAME recompiled_cpu COMMAND cpu_test ${T}/extracted)
set_tests_properties(recompiled_cpu PROPERTIES DEPENDS extract)
add_test(NAME symbols_tool COMMAND ssne-symbols ${T}/test.dol ${T}/test.sym)
