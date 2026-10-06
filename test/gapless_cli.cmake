# SPDX-License-Identifier: Apache-2.0
# End-to-end check of the host CLI: the two halves of a sine, cut inside a FLAC frame, played as
# a folder must reproduce the uncut reference sample for sample.
#   cmake -DCLI=<player_cli> -DPYTHON=<python> -DDATA=<test/data> -DTOOLS=<tools> -DWORK=<dir> -P gapless_cli.cmake
file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK}/album)
configure_file(${DATA}/gap_a.flac "${WORK}/album/01 gap_a.flac" COPYONLY)
configure_file(${DATA}/gap_b.flac "${WORK}/album/02 gap_b.flac" COPYONLY)
execute_process(COMMAND ${CLI} ${WORK}/album ${WORK}/out.wav --gapless-folder RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "player_cli failed: ${rc}")
endif()
execute_process(COMMAND ${PYTHON} ${TOOLS}/gapless_check.py ${WORK}/out.wav ${DATA}/gap_ref.wav RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "output differs from the reference")
endif()
