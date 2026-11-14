#pragma once

/* PICA200 implementation of GLideN64's graphics::ContextImpl (replaces the OpenGL backend).
 * Selected in GLideN64/src/Graphics/Context.cpp under __3DS__. */

#include <Graphics/ContextImpl.h>

namespace pica {

graphics::ContextImpl *createContextImpl();

/* Presents GLideN64's window buffer to the top screen and ends the PICA frame
 * (called from the 3DS DisplayWindow's swapBuffers). */
void presentWindow();

} // namespace pica
