#if defined(__linux__) || defined(__unix__)

#include "ui_window.hpp"
#include "ui_platform.hpp"
#include "../net/network_engine.hpp"
#include "../core/crypto_identity.hpp"
#include "../capture/screen_capture.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <iostream>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace cppdesk {

namespace {

uint16_t mapKeySymToVk(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) {
        return static_cast<uint16_t>(0x41 + (ks - XK_a));
    }
    if (ks >= XK_A && ks <= XK_Z) {
        return static_cast<uint16_t>(0x41 + (ks - XK_A));
    }
    if (ks >= XK_0 && ks <= XK_9) {
        return static_cast<uint16_t>(0x30 + (ks - XK_0));
    }
    switch (ks) {
        case XK_Return:    return 0x0D; // VK_RETURN
        case XK_BackSpace: return 0x08; // VK_BACK
        case XK_Tab:       return 0x09; // VK_TAB
        case XK_Escape:    return 0x1B; // VK_ESCAPE
        case XK_space:     return 0x20; // VK_SPACE
        case XK_Left:      return 0x25; // VK_LEFT
        case XK_Up:        return 0x26; // VK_UP
        case XK_Right:     return 0x27; // VK_RIGHT
        case XK_Down:      return 0x28; // VK_DOWN
        case XK_Delete:    return 0x2E; // VK_DELETE
        case XK_Home:      return 0x24; // VK_HOME
        case XK_End:       return 0x23; // VK_END
        case XK_Page_Up:   return 0x21; // VK_PRIOR
        case XK_Page_Down: return 0x22; // VK_NEXT
        case XK_Shift_L:   return 0x10; // VK_SHIFT
        case XK_Shift_R:   return 0x10;
        case XK_Control_L: return 0x11; // VK_CONTROL
        case XK_Control_R: return 0x11;
        case XK_Alt_L:     return 0x12; // VK_MENU
        case XK_Alt_R:     return 0x12;
        case XK_Super_L:   return 0x5B; // VK_LWIN
        case XK_Super_R:   return 0x5C; // VK_RWIN
        default:           return 0;
    }
}

} // namespace

struct CppDeskWindowLinuxImpl {
    Display* display = nullptr;
    Window window = 0;
    GC gc = nullptr;
    int screen = 0;
    Atom wmDeleteWindow = 0;

    int winWidth = 1280;
    int winHeight = 720;
    bool isClosed = false;

    uint64_t lastFrameSeq = 0;
    std::vector<uint8_t> frameBgra;
    int frameW = 0;
    int frameH = 0;
    CursorState cursorState{};
    ScaleMode scaleMode = ScaleMode::FillAspect;

    std::vector<uint8_t> backbuffer;
    XImage* ximage = nullptr;

    bool hasShm = false;
    XShmSegmentInfo shminfo{};
    XImage* shmImage = nullptr;

    void cleanup() {
        if (hasShm && shmImage) {
            XShmDetach(display, &shminfo);
            shmImage->data = nullptr;
            XDestroyImage(shmImage);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, nullptr);
            shmImage = nullptr;
            hasShm = false;
        } else if (ximage) {
            ximage->data = nullptr;
            XDestroyImage(ximage);
            ximage = nullptr;
        }

        if (gc && display) {
            XFreeGC(display, gc);
            gc = nullptr;
        }
        if (window && display) {
            XDestroyWindow(display, window);
            window = 0;
        }
        if (display) {
            XCloseDisplay(display);
            display = nullptr;
        }
    }

    void allocateBuffer(int w, int h) {
        winWidth = w;
        winHeight = h;

        if (hasShm && shmImage) {
            XShmDetach(display, &shminfo);
            shmImage->data = nullptr;
            XDestroyImage(shmImage);
            shmdt(shminfo.shmaddr);
            shmctl(shminfo.shmid, IPC_RMID, nullptr);
            shmImage = nullptr;
            hasShm = false;
        } else if (ximage) {
            ximage->data = nullptr;
            XDestroyImage(ximage);
            ximage = nullptr;
        }

        Visual* visual = DefaultVisual(display, screen);
        int depth = DefaultDepth(display, screen);

        // Try MIT-SHM first
        if (XShmQueryExtension(display)) {
            shmImage = XShmCreateImage(display, visual, depth, ZPixmap, nullptr, &shminfo, winWidth, winHeight);
            if (shmImage) {
                shminfo.shmid = shmget(IPC_PRIVATE, shmImage->bytes_per_line * shmImage->height, IPC_CREAT | 0777);
                if (shminfo.shmid >= 0) {
                    shminfo.shmaddr = shmImage->data = static_cast<char*>(shmat(shminfo.shmid, nullptr, 0));
                    shminfo.readOnly = False;
                    if (shminfo.shmaddr != reinterpret_cast<char*>(-1) && XShmAttach(display, &shminfo)) {
                        hasShm = true;
                        return;
                    }
                    if (shminfo.shmaddr != reinterpret_cast<char*>(-1)) {
                        shmdt(shminfo.shmaddr);
                    }
                    shmctl(shminfo.shmid, IPC_RMID, nullptr);
                }
                shmImage->data = nullptr;
                XDestroyImage(shmImage);
                shmImage = nullptr;
            }
        }

        // Fallback to standard XImage
        backbuffer.resize(winWidth * winHeight * 4, 0);
        ximage = XCreateImage(display, visual, depth, ZPixmap, 0,
                              reinterpret_cast<char*>(backbuffer.data()),
                              winWidth, winHeight, 32, winWidth * 4);
    }

    void renderFrame() {
        if (!display || !window || !gc || winWidth <= 0 || winHeight <= 0) return;

        char* dstData = hasShm ? shmImage->data : reinterpret_cast<char*>(backbuffer.data());
        int dstPitch = hasShm ? shmImage->bytes_per_line : (winWidth * 4);

        if (frameW <= 0 || frameH <= 0 || frameBgra.empty()) {
            // Paint black/slate background
            for (int y = 0; y < winHeight; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(dstData + y * dstPitch);
                std::fill_n(row, winWidth, 0xFF141416); // Slate dark
            }
        } else {
            ViewportGeometry geo = UiPlatform::calculateViewportGeometry(
                static_cast<float>(winWidth),
                static_cast<float>(winHeight),
                static_cast<uint32_t>(frameW),
                static_cast<uint32_t>(frameH),
                scaleMode
            );

            // Clear letterbox / pillarbox margins
            for (int y = 0; y < winHeight; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(dstData + y * dstPitch);
                std::fill_n(row, winWidth, 0xFF0D0E11);
            }

            int srcPitch = frameW * 4;
            int rLeft = std::clamp(static_cast<int>(geo.contentRect.left), 0, winWidth);
            int rTop = std::clamp(static_cast<int>(geo.contentRect.top), 0, winHeight);
            int rRight = std::clamp(static_cast<int>(geo.contentRect.right), 0, winWidth);
            int rBottom = std::clamp(static_cast<int>(geo.contentRect.bottom), 0, winHeight);

            int drawW = rRight - rLeft;
            int drawH = rBottom - rTop;

            if (drawW > 0 && drawH > 0) {
                float invScaleX = 1.0f / std::max(geo.scaleX, 0.001f);
                float invScaleY = 1.0f / std::max(geo.scaleY, 0.001f);

                for (int y = 0; y < drawH; ++y) {
                    int srcY = std::clamp(static_cast<int>(y * invScaleY), 0, frameH - 1);
                    const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(frameBgra.data() + srcY * srcPitch);
                    uint32_t* dstRow = reinterpret_cast<uint32_t*>(dstData + (rTop + y) * dstPitch + rLeft * 4);

                    for (int x = 0; x < drawW; ++x) {
                        int srcX = std::clamp(static_cast<int>(x * invScaleX), 0, frameW - 1);
                        dstRow[x] = srcRow[srcX];
                    }
                }
            }
        }

        if (hasShm && shmImage) {
            XShmPutImage(display, window, gc, shmImage, 0, 0, 0, 0, winWidth, winHeight, False);
        } else if (ximage) {
            XPutImage(display, window, gc, ximage, 0, 0, 0, 0, winWidth, winHeight);
        }
        XFlush(display);
    }
};

CppDeskWindow::CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget)
    : identity_(identity), network_(network), autoConnectTarget_(autoConnectTarget), pImpl_(std::make_unique<CppDeskWindowLinuxImpl>()) {}

CppDeskWindow::~CppDeskWindow() {
    close();
}

bool CppDeskWindow::create(void* displayContext, int showCommand) {
    (void)showCommand;
    Display* dpy = displayContext ? static_cast<Display*>(displayContext) : XOpenDisplay(nullptr);
    if (!dpy) {
        std::cerr << "CppDesk: Could not open X11 display connection.\n";
        return false;
    }

    pImpl_->display = dpy;
    pImpl_->screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, pImpl_->screen);

    XSetWindowAttributes attrs{};
    attrs.background_pixel = BlackPixel(dpy, pImpl_->screen);
    attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                       StructureNotifyMask | FocusChangeMask;

    pImpl_->window = XCreateWindow(
        dpy, root,
        100, 100, pImpl_->winWidth, pImpl_->winHeight, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWBackPixel | CWEventMask, &attrs
    );

    if (!pImpl_->window) {
        pImpl_->cleanup();
        return false;
    }

    XStoreName(dpy, pImpl_->window, "CppDesk (Linux Native X11)");

    pImpl_->wmDeleteWindow = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, pImpl_->window, &pImpl_->wmDeleteWindow, 1);

    pImpl_->gc = XCreateGC(dpy, pImpl_->window, 0, nullptr);
    pImpl_->allocateBuffer(pImpl_->winWidth, pImpl_->winHeight);

    XMapWindow(dpy, pImpl_->window);
    XFlush(dpy);

    if (!autoConnectTarget_.empty()) {
        network_.connectToRemote(autoConnectTarget_, "");
    }

    return true;
}

int CppDeskWindow::messageLoop() {
    if (!pImpl_ || !pImpl_->display || !pImpl_->window) return 1;

    Display* dpy = pImpl_->display;
    auto lastRenderTime = std::chrono::steady_clock::now();

    while (!pImpl_->isClosed) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);

            switch (ev.type) {
                case ClientMessage: {
                    if (static_cast<Atom>(ev.xclient.data.l[0]) == pImpl_->wmDeleteWindow) {
                        pImpl_->isClosed = true;
                    }
                    break;
                }
                case ConfigureNotify: {
                    if (ev.xconfigure.width != pImpl_->winWidth || ev.xconfigure.height != pImpl_->winHeight) {
                        pImpl_->allocateBuffer(ev.xconfigure.width, ev.xconfigure.height);
                    }
                    break;
                }
                case Expose: {
                    if (ev.xexpose.count == 0) {
                        pImpl_->renderFrame();
                    }
                    break;
                }
                case MotionNotify: {
                    if (pImpl_->frameW > 0 && pImpl_->frameH > 0) {
                        ViewportGeometry geo = UiPlatform::calculateViewportGeometry(
                            static_cast<float>(pImpl_->winWidth),
                            static_cast<float>(pImpl_->winHeight),
                            static_cast<uint32_t>(pImpl_->frameW),
                            static_cast<uint32_t>(pImpl_->frameH),
                            pImpl_->scaleMode
                        );
                        float nx = 0.0f, ny = 0.0f;
                        bool inside = UiPlatform::mapPointToNormalized(
                            static_cast<float>(ev.xmotion.x),
                            static_cast<float>(ev.xmotion.y),
                            geo, nx, ny
                        );
                        if (inside) {
                            network_.sendMouseMove(nx, ny);
                        }
                    }
                    break;
                }
                case ButtonPress:
                case ButtonRelease: {
                    bool isDown = (ev.type == ButtonPress);
                    unsigned int btn = ev.xbutton.button;

                    if (btn == Button4) { // Scroll Up
                        if (isDown) network_.sendMouseWheel(120, 0);
                    } else if (btn == Button5) { // Scroll Down
                        if (isDown) network_.sendMouseWheel(-120, 0);
                    } else {
                        MouseButtonId mBtn = MouseButtonId::Left;
                        if (btn == Button2) mBtn = MouseButtonId::Middle;
                        else if (btn == Button3) mBtn = MouseButtonId::Right;

                        ViewportGeometry geo = UiPlatform::calculateViewportGeometry(
                            static_cast<float>(pImpl_->winWidth),
                            static_cast<float>(pImpl_->winHeight),
                            static_cast<uint32_t>(pImpl_->frameW),
                            static_cast<uint32_t>(pImpl_->frameH),
                            pImpl_->scaleMode
                        );
                        float nx = 0.0f, ny = 0.0f;
                        bool inside = UiPlatform::mapPointToNormalized(
                            static_cast<float>(ev.xbutton.x),
                            static_cast<float>(ev.xbutton.y),
                            geo, nx, ny
                        );
                        if (inside) {
                            network_.sendMouseButton(mBtn, isDown, nx, ny);
                        }
                    }
                    break;
                }
                case KeyPress:
                case KeyRelease: {
                    bool isDown = (ev.type == KeyPress);
                    KeySym ks = XLookupKeysym(&ev.xkey, 0);
                    uint16_t vk = mapKeySymToVk(ks);
                    if (vk != 0) {
                        network_.sendKeyEvent(vk, 0, isDown, false);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Receive any new video frames from network engine
        bool hasNewFrame = network_.copyLatestViewerFrame(
            pImpl_->lastFrameSeq,
            pImpl_->frameBgra,
            pImpl_->frameW,
            pImpl_->frameH,
            pImpl_->cursorState
        );

        auto now = std::chrono::steady_clock::now();
        auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRenderTime).count();

        if (hasNewFrame || elapsedMs >= 33) { // Cap at ~30-60 FPS UI pacing
            pImpl_->renderFrame();
            lastRenderTime = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    return 0;
}

void CppDeskWindow::close() {
    if (pImpl_) {
        pImpl_->cleanup();
    }
}

} // namespace cppdesk

#endif // defined(__linux__) || defined(__unix__)
