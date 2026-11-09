# SPDX-License-Identifier: MIT
# Copyright (c) 2026 BisocM
#
# Copies the first ```c block of a Markdown document into a C file, so the
# quick-start example in README.md is compiled and run by the test suite.
#
# Usage: cmake -DDOCUMENT=<README.md> -DOUTPUT=<example.c> -P extract_example.cmake

file(READ "${DOCUMENT}" _text)
set(_open "```c\n")
string(FIND "${_text}" "${_open}" _start)
if(_start EQUAL -1)
    message(FATAL_ERROR "${DOCUMENT} has no ```c block")
endif()
string(LENGTH "${_open}" _open_length)
math(EXPR _start "${_start} + ${_open_length}")
string(SUBSTRING "${_text}" ${_start} -1 _rest)
string(FIND "${_rest}" "\n```" _end)
if(_end EQUAL -1)
    message(FATAL_ERROR "the ```c block of ${DOCUMENT} is not closed")
endif()
math(EXPR _end "${_end} + 1")
string(SUBSTRING "${_rest}" 0 ${_end} _code)
file(WRITE "${OUTPUT}" "${_code}")
