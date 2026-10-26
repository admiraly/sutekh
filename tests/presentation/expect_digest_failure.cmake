# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
#
# Negative test of space_demo --expect-digest: a wrong expected digest must make the run exit 1 and say why, also
# when the renderer ran (a passing renderer must not hide the digest failure). An offscreen run without
# presentation (no Vulkan) prints "SKIP: no presentation", which the test's SKIP_REGULAR_EXPRESSION turns into a skip.
#
# cmake -DDEMO=<space_demo> -DMODE=headless|offscreen -DOUT=<ppm> -P expect_digest_failure.cmake
set(wrong 0000000000000000000000000000000000000000000000000000000000000000)
if(MODE STREQUAL "offscreen")
    set(args --offscreen "${OUT}" --frames 60 --expect-digest ${wrong})
else()
    set(args --headless --script --frames 60 --expect-digest ${wrong})
endif()
execute_process(COMMAND "${DEMO}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
message("${out}${err}")
if(MODE STREQUAL "offscreen" AND out MATCHES "presentation unavailable")
    message("SKIP: no presentation, so the renderer path was not exercised")
    return()
endif()
if(NOT rc STREQUAL "1")
    message(FATAL_ERROR "expected exit 1 for a wrong --expect-digest, got ${rc}")
endif()
if(NOT out MATCHES "digest differs from --expect-digest")
    message(FATAL_ERROR "the digest failure was not reported")
endif()
if(MODE STREQUAL "offscreen" AND NOT out MATCHES "render: frames")
    message(FATAL_ERROR "the offscreen run did not render")
endif()
message("wrong digest refused (exit 1)")
