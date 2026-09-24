# komi-player (stage 1): included from vendor/tilefinch/CMakeLists.txt through
# KOMI_EXTRA_CMAKE, so it links the same tilefinch_core and transport the
# browser EBOOT does. Outputs, in <build>/komi-player/:
#   komi-player.prx   loaded from host0: by PSPLink (scripts/run_player.sh)
#   EBOOT.PBP         the same program for the Memory Stick
if(NOT PSP OR NOT PSP_BROWSER_LIBCURL_TRANSPORT OR PSP_BROWSER_CURL_STUB)
    return()
endif()

set(KOMI_PLAYER_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(KOMI_PLAYER_OUT "${CMAKE_CURRENT_BINARY_DIR}/komi-player")
file(MAKE_DIRECTORY "${KOMI_PLAYER_OUT}")

# The media session and its presenter live in the browser executable's own
# source list, not in tilefinch_core; compile the same files here.
set(KOMI_PLAYER_SOURCES
    "${KOMI_PLAYER_DIR}/main.c"
    "${KOMI_PLAYER_DIR}/psp_log_enabled.c"
    src/psp_media_buffering.c
    src/psp_media_open.c
    src/psp_media_hls.c
    src/psp_hls_gzip.c
    src/psp_media_present_session.c
    src/psp_media_seek.c
    src/psp_media_session.c
    src/psp_media_telemetry.c
    src/psp_swdec_component.c
    src/psp_network.c
    src/psp_time.c
    src/psp_atomic_shims.c)

foreach(_kind elf prx)
    set(_target komi-player-${_kind})
    add_executable(${_target} ${KOMI_PLAYER_SOURCES})
    set_target_properties(${_target} PROPERTIES
        OUTPUT_NAME komi-player-${_kind}.elf
        RUNTIME_OUTPUT_DIRECTORY "${KOMI_PLAYER_OUT}")
    target_include_directories(${_target} PRIVATE src)
    # TILEFINCH_PSP_LOG_IMPLEMENTATION keeps psp_log_* real in these sources
    # (shipping builds macro them away) without TILEFINCH_PSP_VALIDATION_LOG,
    # which would change struct layouts shared with tilefinch_core.
    target_compile_definitions(${_target} PRIVATE
        TILEFINCH_PSP_LIVE_NETWORK=1 TILEFINCH_PSP_LOG_IMPLEMENTATION=1)
    target_link_options(${_target} PRIVATE "LINKER:--wrap=printf")
    target_link_libraries(${_target} PRIVATE
        tilefinch_core tilefinch_psp_ui tilefinch_psp_display
        tilefinch_psp_media_scale tilefinch_psp_media_present
        tilefinch_psp_app_support
        ${TILEFINCH_PSP_TRANSPORT_LIBRARIES}
        "-Wl,--whole-archive"
        pspnet pspnet_inet pspnet_apctl pspnet_resolver pspwlan psputility
        "-Wl,--no-whole-archive"
        m pspdisplay pspge pspctrl pspgu pspdmac psppower
        pspaudiocodec pspaudio psputility)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.31")
        set_property(TARGET ${_target} PROPERTY
            LINK_LIBRARIES_STRATEGY REORDER_MINIMALLY)
    endif()
    set_property(TARGET ${_target} APPEND PROPERTY LINK_DEPENDS
        "${PSP_BROWSER_GC_KEEP_SCRIPT}")
endforeach()

# PRX: the PSPSDK relocatable-module recipe, as psp-browser-script-dev-prx.
target_link_options(komi-player-prx PRIVATE
    "-specs=${PSPDEV}/psp/sdk/lib/prxspecs"
    "LINKER:-q"
    "LINKER:-T,${PSPDEV}/psp/sdk/lib/linkfile.prx"
    "${PSPDEV}/psp/sdk/lib/prxexports.o")
add_custom_command(TARGET komi-player-prx POST_BUILD
    COMMAND "${PSPDEV}/bin/psp-fixup-imports"
        "${KOMI_PLAYER_OUT}/komi-player-prx.elf"
    COMMAND "${PSPDEV}/bin/psp-prxgen"
        "${KOMI_PLAYER_OUT}/komi-player-prx.elf"
        "${KOMI_PLAYER_OUT}/komi-player.prx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PSP_BROWSER_PSP_CA_BUNDLE}" "${KOMI_PLAYER_OUT}/roots.pem"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${KOMI_PLAYER_DIR}/videos.txt" "${KOMI_PLAYER_OUT}/komi-videos.txt"
    COMMENT "Generating komi-player.prx for PSPLink")

# EBOOT: same objects, packed for the Memory Stick. MEMSIZE=1 is patched in
# by scripts/package.py's set_pbp_title when it is installed.
create_pbp_file(TARGET komi-player-elf
    TITLE "komi-player"
    OUTPUT_DIR "${KOMI_PLAYER_OUT}")
