set(ARTC_PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/AudioPlayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/InputHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GameSelector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

add_executable(artemis_runner ${ARTC_PLATFORM_SOURCES})
target_compile_definitions(artemis_runner PRIVATE SDL_MAIN_HANDLED)
target_link_libraries(artemis_runner PRIVATE artc_core)

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SWITCH_SDL2 REQUIRED IMPORTED_TARGET sdl2)
    pkg_check_modules(SWITCH_SDL2_IMAGE REQUIRED IMPORTED_TARGET SDL2_image)
    pkg_check_modules(SWITCH_SDL2_MIXER REQUIRED IMPORTED_TARGET SDL2_mixer)
    pkg_check_modules(SWITCH_SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

    function(artc_link_switch target)
        target_compile_definitions(${target} PRIVATE __SWITCH__)
        target_link_libraries(${target} PRIVATE
            PkgConfig::SWITCH_SDL2
            PkgConfig::SWITCH_SDL2_IMAGE
            PkgConfig::SWITCH_SDL2_MIXER
            PkgConfig::SWITCH_SDL2_TTF
            nx
        )
    endfunction()

    artc_link_switch(artemis_runner)

    # NRO fallback always starts the multi-game launcher.  It must never probe
    # romfs because Ryujinx throws when a no-romfs NRO opens the romfs service.
    add_executable(artemis_runner_nro ${ARTC_PLATFORM_SOURCES})
    target_compile_definitions(artemis_runner_nro PRIVATE SDL_MAIN_HANDLED ARTC_NRO=1)
    target_link_libraries(artemis_runner_nro PRIVATE artc_core)
    artc_link_switch(artemis_runner_nro)

    set(ARTC_NRO "${CMAKE_BINARY_DIR}/artemis_runner.nro")
    set(ARTC_NACP "${CMAKE_BINARY_DIR}/artemis_runner.nacp")
    add_custom_command(TARGET artemis_runner_nro POST_BUILD
        COMMAND ${NX_NACPTOOL_EXE}
            --create "Artemis Runner" "artemis-runner" "0.1.0"
            ${ARTC_NACP}
        COMMAND ${NX_ELF2NRO_EXE}
            $<TARGET_FILE:artemis_runner_nro> ${ARTC_NRO}
            --nacp=${ARTC_NACP}
        VERBATIM
    )
    add_custom_target(artemis_runner_nro_package ALL DEPENDS artemis_runner_nro)
else()
    find_package(SDL2 CONFIG REQUIRED)
    find_package(SDL2_image CONFIG REQUIRED)
    find_package(SDL2_mixer CONFIG REQUIRED)
    find_package(SDL2_ttf CONFIG REQUIRED)
    target_link_libraries(artemis_runner PRIVATE
        SDL2::SDL2
        SDL2_image::SDL2_image
        SDL2_mixer::SDL2_mixer
        SDL2_ttf::SDL2_ttf
    )
endif()
