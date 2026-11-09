# Portable device core. Builds as an ESP-IDF component or a plain CMake library.
set(HG_CORE_SOURCES
    src/app.cpp
    src/app_settings.cpp
    src/app_camera.cpp
    src/app_panel.cpp
    src/app_power.cpp
    src/app_perf.cpp
    src/app_phone.cpp
    src/app_timer.cpp
    src/canvas.cpp
    src/crypto.cpp
    src/font5x7.cpp
    src/font_cjk.cpp
    src/font_cjk_data.cpp
    src/i18n.cpp
    src/i18n_es.cpp
    src/frame_stats.cpp
    src/json.cpp
    src/mascot.cpp
    src/mascot_data.cpp
    src/panel.cpp
    src/protocol.cpp
    src/qr.cpp
    src/setup.cpp
    src/tones.cpp
    src/touch.cpp
    src/ui.cpp
    src/vad.cpp
    src/wifi_networks.cpp
    # Vendored QR encoder (MIT, Richard Moore / Project Nayuki). Adapted for this
    # project; see NOTICE. Committed as .cpp because the project is CXX-only.
    third_party/qrcode/qrcode.cpp
)

# The 12x12 CJK font and the taller text lines it needs (docs/cjk.md). Everything that
# includes hg/font.hpp must see the same value, so it travels with the core as a PUBLIC definition.
option(HG_CJK_FONT "Build in the 12x12 CJK font and its taller text lines" OFF)

# Panels (hg/panel.hpp): LVGL 9.3 draws them. Its configuration is lvgl/lv_conf.h on every
# build, never ESP-IDF's Kconfig, so the simulator draws what the device draws.
set(HG_PANEL_SOURCES src/panel_lvgl.cpp)
file(GLOB HG_PANEL_FONTS "${CMAKE_CURRENT_SOURCE_DIR}/src/panel_fonts/*.c")
set(HG_LV_CONF "${CMAKE_CURRENT_SOURCE_DIR}/lvgl/lv_conf.h")

if(ESP_PLATFORM)
  # On the ESP32 the LVGL view and its fonts build in the main component, which requires
  # LVGL (esp32/main/CMakeLists.txt): a requirement on a managed component that only some
  # builds have doesn't reach this one. Here HG_PANEL only drops the stubs in panel.cpp.
  idf_component_register(SRCS ${HG_CORE_SOURCES} INCLUDE_DIRS include third_party/qrcode)
  if(HG_CJK_FONT)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HG_CJK_FONT=1)
  endif()
  if(CONFIG_HG_PANEL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HG_PANEL=1)
  endif()
  return()
endif()

# The host build downloads LVGL on the first configure; HG_PANEL=OFF builds without panels.
option(HG_PANEL "Build in LVGL panels (downloads LVGL 9.3.0 on the first configure)" ON)
if(HG_PANEL AND NOT TARGET lvgl)
  enable_language(C)
  include(FetchContent)
  set(LV_BUILD_CONF_PATH "${HG_LV_CONF}" CACHE PATH "" FORCE)
  set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
  set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(lvgl
    URL https://github.com/lvgl/lvgl/archive/refs/tags/v9.3.0.tar.gz
    URL_HASH SHA256=4933becfd3603b29158a5d04138139582836ef2bc17beb6c39dccda9cb0d32e7)
  FetchContent_MakeAvailable(lvgl)
  set_target_properties(lvgl PROPERTIES POSITION_INDEPENDENT_CODE ON)  # linked into the simulator's shared library
  target_compile_definitions(lvgl PUBLIC LV_KCONFIG_IGNORE)
  # The panel fonts are LVGL font descriptors, in C.
  add_library(hg_panel_fonts STATIC ${HG_PANEL_FONTS})
  target_link_libraries(hg_panel_fonts PUBLIC lvgl)
  set_target_properties(hg_panel_fonts PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# hg_core follows HG_CJK_FONT. The host tests also build hg_core_cjk, with the font always in,
# so the CJK code is tested on every build (firmware/CMakeLists.txt).
list(TRANSFORM HG_CORE_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
set(HG_CORE_SOURCES "${HG_CORE_SOURCES}" CACHE INTERNAL "")
set(HG_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "")
set(HG_PANEL_ON "${HG_PANEL}" CACHE INTERNAL "")
set(HG_PANEL_CPP "${CMAKE_CURRENT_SOURCE_DIR}/${HG_PANEL_SOURCES}" CACHE INTERNAL "")
function(hg_core_library name cjk)
  if(HG_PANEL_ON)
    add_library(${name} STATIC ${HG_CORE_SOURCES} ${HG_PANEL_CPP})
    target_compile_definitions(${name} PRIVATE HG_PANEL=1)
    target_link_libraries(${name} PRIVATE hg_panel_fonts lvgl)
  else()
    add_library(${name} STATIC ${HG_CORE_SOURCES})
  endif()
  target_include_directories(${name} PUBLIC ${HG_CORE_DIR}/include)
  if(cjk)
    target_compile_definitions(${name} PUBLIC HG_CJK_FONT=1)
  endif()
  target_include_directories(${name} PRIVATE ${HG_CORE_DIR}/third_party/qrcode)
  target_compile_features(${name} PUBLIC cxx_std_17)
  set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -fno-exceptions -fno-rtti)
  endif()
endfunction()
hg_core_library(hg_core ${HG_CJK_FONT})
