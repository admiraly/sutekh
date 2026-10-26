# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
# Public targets for a project that includes Sutekh with add_subdirectory (README, "Sutekh as a submodule"). Included
# last from CMakeLists.txt, when every library exists. Each Sutekh::<name> alias is a library whose usage requirement
# is include/ only (internal directories such as src/ stay PRIVATE). When Sutekh is not the top-level project, include/
# reaches the consumer as a SYSTEM directory, so the consumer's own warning flags (-Wpedantic -Werror) do not apply
# to engine headers. Optional libraries have an alias only when they are built (su_render needs Vulkan headers,
# su_window the SDL3 headers): check with if(TARGET Sutekh::render).
set(_sutekh_public_libraries
    host su_host                      # include/sutekh/host.h, clock.h, replay.h (static library)
    host_shared sutekh_host           # the same API as a shared library (defines SUTEKH_HOST_DLL for its users)
    input su_input                    # input.h
    window su_window                  # window.h (Linux; SDL3 opened at run time)
    render su_render                  # render.h (Vulkan loader opened at run time)
    render_util su_render_util        # portable render helpers (render.h math, haze, lights, particles)
    asset su_asset                    # asset.h, dds.h (cooked packs)
    asset_import su_asset_import      # glTF importer (a stub that reports the missing importer without cgltf/stb)
    audio su_audio                    # audio.h, audio_asset.h, audio_spatial.h, audio_stream_async.h
    audio_events su_audio_events      # audio_event.h, audio_graph.h, audio_bus.h, audio_parameter.h, ...
    audio_offline su_audio_offline    # audio_offline.h
    audio_inspect su_audio_inspect    # audio_inspect.h
    audio_system su_audio_system)     # audio_system.h
set(_sutekh_public "")
while(_sutekh_public_libraries)
    list(POP_FRONT _sutekh_public_libraries _sutekh_alias _sutekh_target)
    if(TARGET ${_sutekh_target})
        add_library(Sutekh::${_sutekh_alias} ALIAS ${_sutekh_target})
        list(APPEND _sutekh_public Sutekh::${_sutekh_alias})
        if(NOT PROJECT_IS_TOP_LEVEL)
            set_property(TARGET ${_sutekh_target} APPEND PROPERTY
                         INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${SUTEKH_CONTRACT_INCLUDE}")
        endif()
    endif()
endwhile()
if(TARGET sutekh_vulkan_headers)
    list(APPEND _sutekh_public Sutekh::vulkan_headers) # defined in cmake/SutekhPresentation.cmake
endif()
# Programs: the engine (`sutekh serve`, the control plane) and the asset cook tool, for $<TARGET_FILE:...>.
add_executable(Sutekh::sutekh ALIAS sutekh)
add_executable(Sutekh::asset_cook ALIAS asset_cook)
list(APPEND _sutekh_public Sutekh::sutekh Sutekh::asset_cook)
# The list, visible to an add_subdirectory consumer (cache variables are global).
set(SUTEKH_PUBLIC_TARGETS "${_sutekh_public}" CACHE INTERNAL "Sutekh::* targets of this configuration")
unset(_sutekh_public)
unset(_sutekh_public_libraries)
unset(_sutekh_alias)
unset(_sutekh_target)
