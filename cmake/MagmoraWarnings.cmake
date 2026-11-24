# Warnings are errors for everything under dsp/.
function(magmora_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W3 /WX /utf-8)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES NOMINMAX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  endif()
endfunction()
