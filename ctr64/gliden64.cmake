# GLideN64's front half (microcode HLE, gSP/gDP, texture cache, frame buffer emulation) built
# against ctr64's PICA200 backend (src/gfx) instead of the OpenGL backend.
# Mirrors the SOURCES_CXX list of mupen64plus-libretro-nx's Makefile.common, minus everything
# under Graphics/OpenGLContext and GLideNHQ (stubbed by TxFilterStub.cpp).

set(GLN ${NX}/GLideN64/src)

set(CTR64_GLIDEN64_SOURCES
  ${GLN}/3DMath.cpp
  ${GLN}/Combiner.cpp
  ${GLN}/CombinerKey.cpp
  ${GLN}/CommonPluginAPI.cpp
  ${GLN}/Config.cpp
  ${GLN}/convert.cpp
  ${GLN}/CRC_OPT.cpp
  ${GLN}/DebugDump.cpp
  ${GLN}/Debugger.cpp
  ${GLN}/DepthBuffer.cpp
  ${GLN}/DisplayWindow.cpp
  ${GLN}/DisplayLoadProgress.cpp
  ${GLN}/FrameBuffer.cpp
  ${GLN}/FrameBufferInfo.cpp
  ${GLN}/GBI.cpp
  ${GLN}/gDP.cpp
  ${GLN}/GLideN64.cpp
  ${GLN}/gSP.cpp
  ${GLN}/Log.cpp
  ${GLN}/N64.cpp
  ${GLN}/PaletteTexture.cpp
  ${GLN}/Performance.cpp
  ${GLN}/PostProcessor.cpp
  ${GLN}/RDP.cpp
  ${GLN}/RSP.cpp
  ${GLN}/RSP_LoadMatrix.cpp
  ${GLN}/SoftwareRender.cpp
  ${GLN}/TexrectDrawer.cpp
  ${GLN}/TextDrawerStub.cpp
  ${GLN}/TextureFilterHandler.cpp
  ${GLN}/Textures.cpp
  ${GLN}/TxFilterStub.cpp
  ${GLN}/VI.cpp
  ${GLN}/ZlutTexture.cpp
  ${GLN}/common/CommonAPIImpl_common.cpp
  ${GLN}/DepthBufferRender/ClipPolygon.cpp
  ${GLN}/DepthBufferRender/DepthBufferRender.cpp
  ${GLN}/BufferCopy/BlueNoiseTexture.cpp
  ${GLN}/BufferCopy/ColorBufferToRDRAM.cpp
  ${GLN}/BufferCopy/DepthBufferToRDRAM.cpp
  ${GLN}/BufferCopy/RDRAMtoColorBuffer.cpp
  ${GLN}/GraphicsDrawer.cpp
  ${GLN}/Graphics/Context.cpp
  ${GLN}/Graphics/ColorBufferReader.cpp
  ${GLN}/Graphics/CombinerProgram.cpp
  ${GLN}/Graphics/ObjectHandle.cpp
  ${GLN}/mupenplus/MemoryStatus_mupenplus.cpp
  ${GLN}/MupenPlusPluginAPI.cpp
  ${GLN}/mupenplus/MupenPlusAPIImpl.cpp
  ${NX}/custom/GLideN64/mupenplus/Config_mupenplus.cpp
  ${NX}/custom/GLideN64/mupenplus/CommonAPIImpl_mupenplus.cpp
  # PICA200 backend
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/pica_Combiner.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/pica_ContextImpl.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/pica_DisplayWindow.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx/pica_Parameters.cpp)

file(GLOB CTR64_GLIDEN64_UCODES ${GLN}/uCodes/*.cpp)
list(APPEND CTR64_GLIDEN64_SOURCES ${CTR64_GLIDEN64_UCODES})

add_library(ctr64_gliden64 STATIC ${CTR64_GLIDEN64_SOURCES})
target_compile_definitions(ctr64_gliden64 PRIVATE ${CTR64_CORE_DEFINES} MUPENPLUSAPI TXFILTER_LIB)
target_include_directories(ctr64_gliden64 PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gfx
  ${CTR64_CORE_INCLUDES}
  ${GLN}/xxHash)
target_compile_options(ctr64_gliden64 PRIVATE
  -std=gnu++17 -O2 -fsigned-char -ffast-math -fno-strict-aliasing -fomit-frame-pointer
  -fno-short-enums -fpermissive -w)
