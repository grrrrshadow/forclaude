set(BASESET_SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_dos.obg
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_dos_de.obg
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_win.obg
        ${CMAKE_CURRENT_SOURCE_DIR}/no_music.obm
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_dos.obm
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_tto.obm
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_win.obm
        ${CMAKE_CURRENT_SOURCE_DIR}/no_sound.obs
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_dos.obs
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_win.obs
)
set(BASESET_OTHER_SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/openttd.grf
        ${CMAKE_CURRENT_SOURCE_DIR}/opntitle.dat
        ${CMAKE_CURRENT_SOURCE_DIR}/orig_extra.grf
        ${CMAKE_CURRENT_SOURCE_DIR}/../openttd.32.bmp
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenTTD-Sans.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenTTD-Serif.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenTTD-Small.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/OpenTTD-Mono.ttf
)

# openttd.grf and orig_extra.grf are built with yagl, the colleague's, the one
# tool this game's sets are made with (the player's rule: no grfcodec), from
# openttd/openttd.yagl and orig_extra/orig_extra.yagl by openttd/openttd_yagl.py
# and orig_extra/orig_extra_yagl.py, and kept here built, with their hashes.

set(BASESET_EXTRAGRF_FILE ${CMAKE_CURRENT_SOURCE_DIR}/orig_extra.grf)

# Walk over all the baseset files, and generate a command to configure them
foreach(BASESET_SOURCE_FILE IN LISTS BASESET_SOURCE_FILES)
    get_filename_component(BASESET_SOURCE_FILE_NAME "${BASESET_SOURCE_FILE}" NAME)
    set(BASESET_BINARY_FILE "${CMAKE_BINARY_DIR}/baseset/${BASESET_SOURCE_FILE_NAME}")

    get_target_property(LANG_SOURCE_FILES language_files LANG_SOURCE_FILES)

    add_custom_command_timestamp(OUTPUT ${BASESET_BINARY_FILE}
            COMMAND ${CMAKE_COMMAND}
                    -DBASESET_SOURCE_FILE=${BASESET_SOURCE_FILE}
                    -DBASESET_BINARY_FILE=${BASESET_BINARY_FILE}
                    -DBASESET_EXTRAGRF_FILE=${BASESET_EXTRAGRF_FILE}
                    -P ${CMAKE_SOURCE_DIR}/cmake/scripts/Baseset.cmake
                    --
                    ${LANG_SOURCE_FILES}
            MAIN_DEPENDENCY ${BASESET_SOURCE_FILE}
            DEPENDS ${LANG_SOURCE_FILES}
                    ${BASESET_EXTRAGRF_FILE}
                    ${BASESET_EXTRAGRF_FILE}.hash
                    ${CMAKE_SOURCE_DIR}/cmake/scripts/Baseset.cmake
            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
            COMMENT "Generating ${BASESET_SOURCE_FILE_NAME} baseset metadata file"
    )

    list(APPEND BASESET_BINARY_FILES ${BASESET_BINARY_FILE})
endforeach()

# Walk over all the other baseset files, and generate a command to copy them
foreach(BASESET_OTHER_SOURCE_FILE IN LISTS BASESET_OTHER_SOURCE_FILES)
    get_filename_component(BASESET_OTHER_SOURCE_FILE_NAME "${BASESET_OTHER_SOURCE_FILE}" NAME)
    set(BASESET_OTHER_BINARY_FILE "${CMAKE_BINARY_DIR}/baseset/${BASESET_OTHER_SOURCE_FILE_NAME}")

    add_custom_command_timestamp(OUTPUT ${BASESET_OTHER_BINARY_FILE}
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${BASESET_OTHER_SOURCE_FILE}
                    ${BASESET_OTHER_BINARY_FILE}
            MAIN_DEPENDENCY ${BASESET_OTHER_SOURCE_FILE}
            COMMENT "Copying ${BASESET_OTHER_SOURCE_FILE_NAME} baseset file"
    )

    list(APPEND BASESET_BINARY_FILES ${BASESET_OTHER_BINARY_FILE})
endforeach()

# The game's own sets (AppendBuiltinGRFs() in src/newgrf_config.cpp): every
# NewGRF in decouple/ goes into every new game, whatever it is called and
# whatever its GRF ID, so a set put there needs no change here.
file(GLOB BASESET_BUILTIN_GRF_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/decouple/*.grf)
foreach(BUILTIN_GRF_FILE IN LISTS BASESET_BUILTIN_GRF_FILES)
    get_filename_component(BUILTIN_GRF_NAME "${BUILTIN_GRF_FILE}" NAME)
    set(BUILTIN_GRF_BINARY_FILE "${CMAKE_BINARY_DIR}/baseset/decouple/${BUILTIN_GRF_NAME}")

    add_custom_command_timestamp(OUTPUT ${BUILTIN_GRF_BINARY_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/baseset/decouple
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${BUILTIN_GRF_FILE}
                    ${BUILTIN_GRF_BINARY_FILE}
            MAIN_DEPENDENCY ${BUILTIN_GRF_FILE}
            COMMENT "Copying ${BUILTIN_GRF_NAME}, one of the game's own sets"
    )

    list(APPEND BASESET_BINARY_FILES ${BUILTIN_GRF_BINARY_FILE})
endforeach()

# Graphics of the game's own (AppendStaticGRFConfigs() in src/newgrf_config.cpp):
# every NewGRF in decouple/grafika/ is static in every game, savegames included.
# Empty now: the 8x pictures it carried while openttd.grf could not (grfcodec
# knew no 8x) are in openttd.grf itself since it is built with yagl.
file(GLOB BASESET_BUILTIN_GRAPHICS_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/decouple/grafika/*.grf)
foreach(BUILTIN_GRAPHICS_FILE IN LISTS BASESET_BUILTIN_GRAPHICS_FILES)
    get_filename_component(BUILTIN_GRAPHICS_NAME "${BUILTIN_GRAPHICS_FILE}" NAME)
    set(BUILTIN_GRAPHICS_BINARY_FILE "${CMAKE_BINARY_DIR}/baseset/decouple/grafika/${BUILTIN_GRAPHICS_NAME}")

    add_custom_command_timestamp(OUTPUT ${BUILTIN_GRAPHICS_BINARY_FILE}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/baseset/decouple/grafika
            COMMAND ${CMAKE_COMMAND} -E copy
                    ${BUILTIN_GRAPHICS_FILE}
                    ${BUILTIN_GRAPHICS_BINARY_FILE}
            MAIN_DEPENDENCY ${BUILTIN_GRAPHICS_FILE}
            COMMENT "Copying ${BUILTIN_GRAPHICS_NAME}, graphics of the game's own"
    )

    list(APPEND BASESET_BINARY_FILES ${BUILTIN_GRAPHICS_BINARY_FILE})
endforeach()

# Create a new target which generates all baseset metadata files
add_custom_target_timestamp(baseset_files
        DEPENDS
        ${BASESET_BINARY_FILES}
)

add_library(basesets
        INTERFACE
)
add_dependencies(basesets
        baseset_files
)
add_library(openttd::basesets ALIAS basesets)
