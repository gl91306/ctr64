/* Definitions of GLideN64's graphics:: parameter constants for the PICA backend
 * (replaces Graphics/OpenGLContext/opengl_Parameters.cpp). */

#include <Graphics/Parameters.h>

#include "pica_params.h"

using namespace pica_param;

namespace graphics {

namespace colorFormat {
ColorFormatParam RED_GREEN_BLUE(CF_RGB);
ColorFormatParam RGBA(CF_RGBA);
ColorFormatParam RG(CF_RG);
ColorFormatParam RED(CF_RED);
ColorFormatParam DEPTH(CF_DEPTH);
ColorFormatParam LUMINANCE(CF_LUMINANCE);
} // namespace colorFormat

namespace internalcolorFormat {
InternalColorFormatParam NOCOLOR(IF_NOCOLOR);
InternalColorFormatParam RGB8(IF_RGB8);
InternalColorFormatParam RGBA8(IF_RGBA8);
InternalColorFormatParam RGBA4(IF_RGBA4);
InternalColorFormatParam RGB5_A1(IF_RGB5_A1);
InternalColorFormatParam RG(IF_RG);
InternalColorFormatParam R16F(IF_R16F);
InternalColorFormatParam DEPTH(IF_DEPTH);
InternalColorFormatParam RG32F(IF_RG32F);
InternalColorFormatParam LUMINANCE(IF_LUMINANCE);
InternalColorFormatParam COLOR_INDEX8(IF_COLOR_INDEX8);
} // namespace internalcolorFormat

namespace datatype {
DatatypeParam UNSIGNED_BYTE(DT_UNSIGNED_BYTE);
DatatypeParam UNSIGNED_SHORT(DT_UNSIGNED_SHORT);
DatatypeParam UNSIGNED_INT(DT_UNSIGNED_INT);
DatatypeParam FLOAT(DT_FLOAT);
DatatypeParam UNSIGNED_SHORT_5_6_5(DT_US_565);
DatatypeParam UNSIGNED_SHORT_5_5_5_1(DT_US_5551);
DatatypeParam UNSIGNED_SHORT_4_4_4_4(DT_US_4444);
} // namespace datatype

namespace textureTarget {
TextureTargetParam TEXTURE_2D(TT_2D);
TextureTargetParam TEXTURE_2D_MULTISAMPLE(TT_2D_MS);
TextureTargetParam RENDERBUFFER(TT_RENDERBUFFER);
TextureTargetParam TEXTURE_EXTERNAL(TT_EXTERNAL);
} // namespace textureTarget

namespace bufferTarget {
BufferTargetParam FRAMEBUFFER(BT_FRAMEBUFFER);
BufferTargetParam DRAW_FRAMEBUFFER(BT_DRAW);
BufferTargetParam READ_FRAMEBUFFER(BT_READ);
} // namespace bufferTarget

namespace bufferAttachment {
BufferAttachmentParam COLOR_ATTACHMENT0(BA_COLOR0);
BufferAttachmentParam COLOR_ATTACHMENT1(BA_COLOR1);
BufferAttachmentParam COLOR_ATTACHMENT2(BA_COLOR2);
BufferAttachmentParam COLOR_ATTACHMENT3(BA_COLOR3);
BufferAttachmentParam COLOR_ATTACHMENT4(BA_COLOR4);
BufferAttachmentParam DEPTH_ATTACHMENT(BA_DEPTH);
} // namespace bufferAttachment

namespace enable {
EnableParam BLEND(EN_BLEND);
EnableParam CULL_FACE(EN_CULL_FACE);
EnableParam DEPTH_TEST(EN_DEPTH_TEST);
EnableParam DEPTH_CLAMP(EN_DEPTH_CLAMP);
EnableParam CLIP_DISTANCE0(EN_CLIP_DISTANCE0);
EnableParam DITHER(EN_DITHER);
EnableParam POLYGON_OFFSET_FILL(EN_POLYGON_OFFSET_FILL);
EnableParam SCISSOR_TEST(EN_SCISSOR_TEST);
} // namespace enable

namespace textureIndices {
TextureUnitParam Tex[2] = {TextureUnitParam(TU_TEX0), TextureUnitParam(TU_TEX1)};
TextureUnitParam DepthTex(TU_DEPTH);
TextureUnitParam ZLUTTex(TU_ZLUT);
TextureUnitParam PaletteTex(TU_PALETTE);
TextureUnitParam MSTex[2] = {TextureUnitParam(TU_MS0), TextureUnitParam(TU_MS1)};
} // namespace textureIndices

namespace textureImageUnits {
ImageUnitParam DepthZ(IU_DEPTH_Z);
ImageUnitParam DepthDeltaZ(IU_DEPTH_DELTA_Z);
ImageUnitParam DepthZCopy(IU_DEPTH_Z_COPY);
ImageUnitParam DepthDeltaZCopy(IU_DEPTH_DELTA_Z_COPY);
} // namespace textureImageUnits

namespace textureImageAccessMode {
ImageAccessModeParam READ_ONLY(AM_READ_ONLY);
ImageAccessModeParam WRITE_ONLY(AM_WRITE_ONLY);
ImageAccessModeParam READ_WRITE(AM_READ_WRITE);
} // namespace textureImageAccessMode

namespace textureParameters {
TextureParam FILTER_NEAREST(TP_NEAREST);
TextureParam FILTER_LINEAR(TP_LINEAR);
TextureParam FILTER_NEAREST_MIPMAP_NEAREST(TP_NEAREST_MIP_NEAREST);
TextureParam FILTER_LINEAR_MIPMAP_NEAREST(TP_LINEAR_MIP_NEAREST);
TextureParam WRAP_CLAMP_TO_EDGE(TP_CLAMP);
TextureParam WRAP_REPEAT(TP_REPEAT);
TextureParam WRAP_MIRRORED_REPEAT(TP_MIRROR);
} // namespace textureParameters

namespace cullMode {
CullModeParam FRONT(CM_FRONT);
CullModeParam BACK(CM_BACK);
CullModeParam FRONT_AND_BACK(CM_FRONT_AND_BACK);
} // namespace cullMode

namespace compare {
CompareParam LEQUAL(CMP_LEQUAL);
CompareParam LESS(CMP_LESS);
CompareParam ALWAYS(CMP_ALWAYS);
} // namespace compare

namespace blend {
BlendParam ZERO(BL_ZERO);
BlendParam ONE(BL_ONE);
BlendParam SRC_ALPHA(BL_SRC_ALPHA);
BlendParam DST_ALPHA(BL_DST_ALPHA);
BlendParam ONE_MINUS_SRC_ALPHA(BL_ONE_MINUS_SRC_ALPHA);
BlendParam CONSTANT_ALPHA(BL_CONSTANT_ALPHA);
BlendParam ONE_MINUS_CONSTANT_ALPHA(BL_ONE_MINUS_CONSTANT_ALPHA);
BlendParam SRC1_COLOR(BL_SRC1_COLOR);
BlendParam ONE_MINUS_SRC1_COLOR(BL_ONE_MINUS_SRC1_COLOR);
BlendParam SRC1_ALPHA(BL_SRC1_ALPHA);
BlendParam ONE_MINUS_SRC1_ALPHA(BL_ONE_MINUS_SRC1_ALPHA);
} // namespace blend

namespace drawmode {
DrawModeParam TRIANGLES(DM_TRIANGLES);
DrawModeParam TRIANGLE_STRIP(DM_TRIANGLE_STRIP);
DrawModeParam TRIANGLE_FAN(DM_TRIANGLE_FAN);
DrawModeParam LINES(DM_LINES);
} // namespace drawmode

namespace blitMask {
BlitMaskParam COLOR_BUFFER(BM_COLOR);
BlitMaskParam DEPTH_BUFFER(BM_DEPTH);
BlitMaskParam STENCIL_BUFFER(BM_STENCIL);
} // namespace blitMask

} // namespace graphics
