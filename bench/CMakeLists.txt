# Stand-alone kernel benchmark (docs/MODEL.md section 3). Not product code, so warnings are not errors.
add_executable(kernel_bench kernel.cpp)
add_executable(kernel_bench_tables kernel.cpp)
target_compile_definitions(kernel_bench_tables PRIVATE USE_TABLES)
if(MSVC)
  target_compile_definitions(kernel_bench PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(kernel_bench_tables PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()
