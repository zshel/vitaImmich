cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(vitaImmich C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Immich")
set(VITA_TITLEID  "VIMM00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -O2")

add_executable(${PROJECT_NAME}
  src/main.c
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  curl
  ssl
  crypto
  zstd
  freetype
  png
  jpeg
  z
  m
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  ScePgf_stub
  SceAppMgr_stub
  SceNet_stub
  SceNetCtl_stub
  SceAvPlayer_stub
  SceAudio_stub
)

# UNSAFE: reading the camera folders (ux0:picture, ux0:video) is outside
# the safe-homebrew sandbox
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/assets/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/assets/Overpass-SemiBold.ttf font.ttf
  FILE ${CMAKE_SOURCE_DIR}/assets/OFL-Overpass.txt OFL-Overpass.txt
)
