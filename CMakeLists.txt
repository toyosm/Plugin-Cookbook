# Plugin Cookbook — builds every recipe into VST3 / CLAP / AU / LV2 / standalone with DPF.
# SPDX-License-Identifier: MIT
#
#   cmake -S . -B build && cmake --build build --parallel
#
# Options:
#   -DCOOKBOOK_RECIPES="a.plugin.toml;b.plugin.toml"   build only these (default: plugins/, or examples/ if empty)
#   -DCOOKBOOK_FORMATS="vst3;clap"                      subset of: standalone lv2 vst3 clap au jack (au is macOS only;
#                                                       jack is DPF's own standalone, default devices at 48 kHz only)
#   -DCOOKBOOK_UI=OFF                                   no editor (hosts show generic sliders; no standalone app)

cmake_minimum_required(VERSION 3.15)

project(PluginCookbook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(COOKBOOK_RECIPES "" CACHE STRING "Recipe files to build (empty = all in plugins/, or examples/ when plugins/ is empty)")
set(COOKBOOK_FORMATS "standalone;lv2;vst3;clap;au" CACHE STRING "Plugin formats to build")
option(COOKBOOK_UI "Build the generic editor UI" ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dpf/cmake/DPF-plugin.cmake")
  message(FATAL_ERROR "DPF is missing. Run: git submodule update --init --recursive")
endif()

add_subdirectory(dpf)

# ---------------------------------------------------------------------------------------------------------------------
# Generate sources from the recipes

set(COOKBOOK_GEN_DIR "${CMAKE_BINARY_DIR}/generated")

find_program(COOKBOOK_PYTHON NAMES python3 python REQUIRED)

set(_gen_args generate --out "${COOKBOOK_GEN_DIR}")
if(COOKBOOK_RECIPES)
  list(APPEND _gen_args ${COOKBOOK_RECIPES})
else()
  list(APPEND _gen_args --all)
endif()
if(NOT COOKBOOK_UI)
  list(APPEND _gen_args --no-ui)
endif()

execute_process(
  COMMAND "${COOKBOOK_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/recipe.py" ${_gen_args}
  RESULT_VARIABLE _gen_result)
if(NOT _gen_result EQUAL 0)
  message(FATAL_ERROR "Recipe generation failed (see errors above). Fix the recipe and re-run cmake.")
endif()

include("${COOKBOOK_GEN_DIR}/plugins.cmake")

# Re-run generation whenever a recipe, the tool, or the set of recipe files changes.
file(GLOB _recipe_globs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*.plugin.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/examples/*.plugin.toml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${COOKBOOK_RECIPE_FILES}
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/recipe.py")

# ---------------------------------------------------------------------------------------------------------------------
# One DPF plugin per recipe

set(_framework "${CMAKE_CURRENT_SOURCE_DIR}/framework")
set(_widgets "${CMAKE_CURRENT_SOURCE_DIR}/dpf-widgets/opengl")
set(_rtaudio "${CMAKE_CURRENT_SOURCE_DIR}/dpf/distrho/src/jackbridge/rtaudio")

# "standalone" is our own app (framework/src/CookbookStandalone.cpp); everything else goes to DPF.
set(_dpf_formats ${COOKBOOK_FORMATS})
list(REMOVE_ITEM _dpf_formats standalone)
set(_standalone OFF)
if("standalone" IN_LIST COOKBOOK_FORMATS)
  if(COOKBOOK_UI)
    set(_standalone ON)
  else()
    message(WARNING "Plugin Cookbook: the standalone app needs the UI; skipping it because COOKBOOK_UI=OFF")
  endif()
endif()

if(_standalone)
  find_package(Threads REQUIRED)
  set(_rtaudio_defs "")
  set(_rtaudio_libs "")
  if(APPLE)
    find_library(APPLE_COREAUDIO_FRAMEWORK CoreAudio)
    find_library(APPLE_COREFOUNDATION_FRAMEWORK CoreFoundation)
    list(APPEND _rtaudio_defs __MACOSX_CORE__)
    list(APPEND _rtaudio_libs "${APPLE_COREAUDIO_FRAMEWORK}" "${APPLE_COREFOUNDATION_FRAMEWORK}")
  elseif(WIN32)
    list(APPEND _rtaudio_defs __WINDOWS_WASAPI__ NOMINMAX WIN32_LEAN_AND_MEAN)
    list(APPEND _rtaudio_libs ksuser mfplat mfuuid ole32 winmm wmcodecdspuuid)
  else()
    find_package(ALSA REQUIRED)
    list(APPEND _rtaudio_defs __LINUX_ALSA__)
    list(APPEND _rtaudio_libs ALSA::ALSA)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE_SIMPLE IMPORTED_TARGET libpulse-simple)
    if(PULSE_SIMPLE_FOUND)
      list(APPEND _rtaudio_defs __LINUX_PULSE__)
      list(APPEND _rtaudio_libs PkgConfig::PULSE_SIMPLE)
    endif()
  endif()
  set_source_files_properties("${_rtaudio}/RtAudio.cpp" PROPERTIES COMPILE_OPTIONS "$<IF:$<BOOL:${MSVC}>,/w,-w>")
endif()

foreach(_id IN LISTS COOKBOOK_PLUGIN_IDS)
  if(COOKBOOK_UI)
    dpf_add_plugin(${_id}
      TARGETS ${_dpf_formats}
      UI_TYPE opengl
      FILES_DSP "${_framework}/src/CookbookPlugin.cpp"
      FILES_UI "${_framework}/src/CookbookUI.cpp" "${_widgets}/DearImGui.cpp")
  else()
    dpf_add_plugin(${_id}
      TARGETS ${_dpf_formats}
      FILES_DSP "${_framework}/src/CookbookPlugin.cpp")
  endif()

  target_include_directories(${_id} PUBLIC
    "${COOKBOOK_GEN_DIR}/${_id}"
    "${_framework}/include"
    "${_widgets}")

  if(_standalone)
    set(_app ${_id}-standalone)
    add_executable(${_app}
      "${_framework}/src/CookbookStandalone.cpp"
      "${_widgets}/DearImGui.cpp"
      "${_rtaudio}/RtAudio.cpp")
    target_include_directories(${_app} PRIVATE
      "${COOKBOOK_GEN_DIR}/${_id}"
      "${_framework}/include"
      "${_widgets}"
      "${_rtaudio}"
      "${CMAKE_CURRENT_SOURCE_DIR}/dpf/distrho")
    target_compile_definitions(${_app} PRIVATE ${_rtaudio_defs})
    target_link_libraries(${_app} PRIVATE
      dgl-opengl
      dgl-opengl-definitions
      dgl-system-libs-definitions
      dgl-system-libs
      Threads::Threads
      ${_rtaudio_libs})
    set_target_properties(${_app} PROPERTIES
      OUTPUT_NAME "${_id}"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

    if(APPLE)
      # A proper .app, with a microphone usage description so macOS lets it open the audio input,
      # and an ad-hoc signature.
      set_target_properties(${_app} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${_framework}/cmake/StandaloneInfo.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "${COOKBOOK_NAME_${_id}}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${COOKBOOK_BUNDLE_ID_${_id}}.standalone"
        MACOSX_BUNDLE_BUNDLE_VERSION "${COOKBOOK_VERSION_${_id}}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${COOKBOOK_VERSION_${_id}}")
      # xattr first: codesign rejects Finder info, which iCloud-synced folders attach to bundles.
      add_custom_command(TARGET ${_app} POST_BUILD
        COMMAND xattr -cr "$<TARGET_BUNDLE_DIR:${_app}>"
        COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${_app}>"
        VERBATIM)
    elseif(WIN32)
      set_target_properties(${_app} PROPERTIES WIN32_EXECUTABLE TRUE)
      if(MSVC)
        target_link_options(${_app} PRIVATE /ENTRY:mainCRTStartup)
      elseif(MINGW)
        target_link_options(${_app} PRIVATE -static)
      endif()
    endif()
  endif()
endforeach()

message(STATUS "Plugin Cookbook: building ${COOKBOOK_PLUGIN_IDS} as ${COOKBOOK_FORMATS}")
