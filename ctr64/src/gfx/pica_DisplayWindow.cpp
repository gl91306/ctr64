/* GLideN64 DisplayWindow for the 3DS: a fixed 400x240 window on the top screen. The PICA
 * backend owns the window buffer; swapping presents it (see pica_ContextImpl.cpp). */

#include <cstdlib>
#include <cstring>

#include <DisplayWindow.h>
#include <Graphics/Context.h>

#include "pica_ContextImpl.h"

class DisplayWindowCtr : public DisplayWindow {
public:
    DisplayWindowCtr() {}

private:
    bool _start() override
    {
        m_bFullscreen = true;
        m_screenWidth = m_width = 400;
        m_screenHeight = m_height = 240;
        _setBufferSize();
        return true;
    }
    void _stop() override {}
    void _restart() override {}
    void _swapBuffers() override { pica::presentWindow(); }
    void _saveScreenshot() override {}
    void _saveBufferContent(graphics::ObjectHandle, CachedTexture *) override {}
    bool _resizeWindow() override { return true; }
    void _changeWindow() override {}
    void _readScreen(void **_pDest, long *_pWidth, long *_pHeight) override
    {
        *_pWidth = m_width;
        *_pHeight = m_height;
        *_pDest = calloc(m_width * m_height, 3);
    }
    void _readScreen2(void *_dest, int *_width, int *_height, int) override
    {
        if (!_width || !_height)
            return;
        *_width = m_screenWidth;
        *_height = m_screenHeight;
        if (_dest)
            memset(_dest, 0, m_screenWidth * m_screenHeight * 3);
    }
    graphics::ObjectHandle _getDefaultFramebuffer() override { return graphics::ObjectHandle::null; }
};

DisplayWindow &DisplayWindow::get()
{
    static DisplayWindowCtr video;
    return video;
}
