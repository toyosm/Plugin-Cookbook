# Plugin Cookbook — builds every recipe into VST3 / CLAP / AU / LV2 / standalone with DPF.
# SPDX-License-Identifier: MIT
#
#   cmake -S . -B build && cmake --build build --parallel
#
# Options:
#   -DCOOKBOOK_RECIPES="a.plugin.toml;b.plugin.toml"   build only these (default: plugins/, or examples/ if empty)
#   -DCOOKBOOK_FORMATS="vst3;clap"                      subset of: jack lv2 vst3 clap au (au is macOS only)
#   -DCOOKBOOK_UI=OFF                                   no editor (hosts show generic sliders; standalone has no UI)

cmake_minimum_required(VERSION 3.15)

project(PluginCookbook LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(COOKBOOK_RECIPES "" CACHE STRING "Recipe files to build (empty = all in plugins/, or examples/ when plugins/ is empty)")
set(COOKBOOK_FORMATS "jack;lv2;vst3;clap;au" CACHE STRING "Plugin formats to build")
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

foreach(_id IN LISTS COOKBOOK_PLUGIN_IDS)
  if(COOKBOOK_UI)
    dpf_add_plugin(${_id}
      TARGETS ${COOKBOOK_FORMATS}
      UI_TYPE opengl
      FILES_DSP "${_framework}/src/CookbookPlugin.cpp"
      FILES_UI "${_framework}/src/CookbookUI.cpp" "${_widgets}/DearImGui.cpp")
  else()
    dpf_add_plugin(${_id}
      TARGETS ${COOKBOOK_FORMATS}
      FILES_DSP "${_framework}/src/CookbookPlugin.cpp")
  endif()

  target_include_directories(${_id} PUBLIC
    "${COOKBOOK_GEN_DIR}/${_id}"
    "${_framework}/include"
    "${_widgets}")
endforeach()

message(STATUS "Plugin Cookbook: building ${COOKBOOK_PLUGIN_IDS} as ${COOKBOOK_FORMATS}")
