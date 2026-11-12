#include "theme.hpp"          // before the X headers, whose Success macro breaks protocol.hpp
#include "canvas_cairo.hpp"

#include "x11_tray.hpp"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstring>

namespace cppdesk {

namespace {

constexpr long kRequestDock = 0;
constexpr int kItems = 4;   // status line, Show, Mute/Unmute, Quit

// Draws `draw` into an ARGB surface the size of `win` and puts it on the window.
template <typename Draw>
void paintWindow(Display* dpy, Window win, Visual* visual, int depth, int w, int h, Draw draw) {
    if (w <= 0 || h <= 0) return;
    cairo_surface_t* s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    draw(s);
    cairo_surface_flush(s);
    XImage* img = XCreateImage(dpy, visual, depth, ZPixmap, 0, reinterpret_cast<char*>(cairo_image_surface_get_data(s)),
                               w, h, 32, cairo_image_surface_get_stride(s));
    if (img) {
        GC gc = XCreateGC(dpy, win, 0, nullptr);
        XPutImage(dpy, win, gc, img, 0, 0, 0, 0, w, h);
        XFreeGC(dpy, gc);
        img->data = nullptr;   // owned by the cairo surface
        XDestroyImage(img);
    }
    cairo_surface_destroy(s);
    XFlush(dpy);
}

} // namespace

X11Tray::X11Tray(Display* dpy, int screen, float scale, std::vector<uint8_t> iconBgra, int iconW, int iconH)
    : dpy_(dpy), screen_(screen), scale_(scale), iconBgra_(std::move(iconBgra)), iconW_(iconW), iconH_(iconH) {
    const std::string sel = "_NET_SYSTEM_TRAY_S" + std::to_string(screen_);
    selection_ = XInternAtom(dpy_, sel.c_str(), False);
    opcode_ = XInternAtom(dpy_, "_NET_SYSTEM_TRAY_OPCODE", False);
    manager_ = XInternAtom(dpy_, "MANAGER", False);
    visualAtom_ = XInternAtom(dpy_, "_NET_SYSTEM_TRAY_VISUAL", False);
    xembedInfo_ = XInternAtom(dpy_, "_XEMBED_INFO", False);
    // A panel that starts later announces itself with a MANAGER message on the root window.
    XSelectInput(dpy_, RootWindow(dpy_, screen_), StructureNotifyMask);
    dock();
}

X11Tray::~X11Tray() {
    undock();
}

void X11Tray::dock() {
    managerWin_ = XGetSelectionOwner(dpy_, selection_);
    if (!managerWin_) return;
    XSelectInput(dpy_, managerWin_, StructureNotifyMask);   // DestroyNotify when the panel goes away

    // Use the panel's ARGB visual when it offers one, so the icon blends into the panel.
    iconVisual_ = DefaultVisual(dpy_, screen_);
    iconDepth_ = DefaultDepth(dpy_, screen_);
    Atom type = 0;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(dpy_, managerWin_, visualAtom_, 0, 1, False, XA_VISUALID, &type, &format, &count, &after,
                           &data) == Success && data && count == 1) {
        XVisualInfo tmpl{};
        tmpl.visualid = static_cast<VisualID>(*reinterpret_cast<unsigned long*>(data));
        int n = 0;
        XVisualInfo* vi = XGetVisualInfo(dpy_, VisualIDMask, &tmpl, &n);
        if (vi && n > 0 && vi->depth == 32) {
            iconVisual_ = vi->visual;
            iconDepth_ = 32;
        }
        if (vi) XFree(vi);
    }
    if (data) XFree(data);

    XSetWindowAttributes attrs{};
    unsigned long mask = CWEventMask;
    attrs.event_mask = ExposureMask | ButtonPressMask | StructureNotifyMask;
    if (iconDepth_ == 32) {
        iconColormap_ = XCreateColormap(dpy_, RootWindow(dpy_, screen_), iconVisual_, AllocNone);
        attrs.colormap = iconColormap_;
        attrs.border_pixel = 0;
        attrs.background_pixel = 0;
        mask |= CWColormap | CWBorderPixel | CWBackPixel;
    } else {
        attrs.background_pixmap = ParentRelative;
        mask |= CWBackPixmap;
    }
    iconSize_ = std::max(16, static_cast<int>(22 * scale_));
    icon_ = XCreateWindow(dpy_, RootWindow(dpy_, screen_), 0, 0, iconSize_, iconSize_, 0, iconDepth_, InputOutput,
                          iconVisual_, mask, &attrs);
    const long info[2] = { 0, 1 };   // XEmbed version 0, mapped
    XChangeProperty(dpy_, icon_, xembedInfo_, xembedInfo_, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(info), 2);
    if (!tooltip_.empty()) XStoreName(dpy_, icon_, tooltip_.c_str());

    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = managerWin_;
    ev.xclient.message_type = opcode_;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = kRequestDock;
    ev.xclient.data.l[2] = static_cast<long>(icon_);
    XSendEvent(dpy_, managerWin_, False, NoEventMask, &ev);
    XFlush(dpy_);
}

void X11Tray::undock() {
    closeMenu();
    if (icon_) XDestroyWindow(dpy_, icon_);
    if (iconColormap_) XFreeColormap(dpy_, iconColormap_);
    icon_ = 0;
    iconColormap_ = 0;
    managerWin_ = 0;
    XFlush(dpy_);
}

void X11Tray::setTooltip(const std::string& text) {
    tooltip_ = text;
    if (icon_) {
        XStoreName(dpy_, icon_, text.c_str());
        XFlush(dpy_);
    }
}

void X11Tray::drawIcon() {
    if (!icon_ || iconBgra_.empty()) return;
    Window root = 0;
    int x = 0, y = 0;
    unsigned int w = 0, h = 0, border = 0, depth = 0;
    XGetGeometry(dpy_, icon_, &root, &x, &y, &w, &h, &border, &depth);
    paintWindow(dpy_, icon_, iconVisual_, iconDepth_, static_cast<int>(w), static_cast<int>(h), [&](cairo_surface_t* s) {
        cairo_t* cr = cairo_create(s);
        if (iconDepth_ != 32) {   // no alpha on the panel: sit on the theme's navigation colour
            cairo_set_source_rgb(cr, COL_BG_NAV.r, COL_BG_NAV.g, COL_BG_NAV.b);
            cairo_paint(cr);
        }
        cairo_surface_t* src = cairo_image_surface_create_for_data(iconBgra_.data(), CAIRO_FORMAT_ARGB32, iconW_,
                                                                   iconH_, iconW_ * 4);
        const double k = std::min(static_cast<double>(w) / iconW_, static_cast<double>(h) / iconH_);
        cairo_translate(cr, (w - iconW_ * k) / 2.0, (h - iconH_ * k) / 2.0);
        cairo_scale(cr, k, k);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_surface_destroy(src);
        cairo_destroy(cr);
    });
}

void X11Tray::openMenu(int rootX, int rootY, const MenuInfo& info) {
    closeMenu();
    labels_ = { info.status.empty() ? "CppDesk" : info.status, "Show CppDesk",
                info.muted ? "Unmute notifications" : "Mute notifications", "Quit" };
    menuW_ = static_cast<int>(230 * scale_);
    menuH_ = static_cast<int>((8 + 28 * kItems) * scale_);
    const int screenW = DisplayWidth(dpy_, screen_), screenH = DisplayHeight(dpy_, screen_);
    const int x = std::clamp(rootX, 0, std::max(0, screenW - menuW_));
    const int y = rootY + menuH_ > screenH ? std::max(0, rootY - menuH_) : rootY;
    XSetWindowAttributes attrs{};
    attrs.override_redirect = True;
    attrs.event_mask = ExposureMask | ButtonPressMask | PointerMotionMask | KeyPressMask;
    menu_ = XCreateWindow(dpy_, RootWindow(dpy_, screen_), x, y, menuW_, menuH_, 0, CopyFromParent, InputOutput,
                          CopyFromParent, CWOverrideRedirect | CWEventMask, &attrs);
    XStoreName(dpy_, menu_, "CppDesk tray menu");
    XMapRaised(dpy_, menu_);
    // Clicks anywhere come to the menu while it is open, so a click outside closes it.
    XGrabPointer(dpy_, menu_, True, ButtonPressMask | PointerMotionMask, GrabModeAsync, GrabModeAsync, None, None,
                 CurrentTime);
    XGrabKeyboard(dpy_, menu_, True, GrabModeAsync, GrabModeAsync, CurrentTime);
    hover_ = -1;
    XFlush(dpy_);
}

void X11Tray::closeMenu() {
    if (!menu_) return;
    XUngrabPointer(dpy_, CurrentTime);
    XUngrabKeyboard(dpy_, CurrentTime);
    XDestroyWindow(dpy_, menu_);
    menu_ = 0;
    XFlush(dpy_);
}

int X11Tray::itemAt(int y) const {
    const int row = static_cast<int>((y - 4 * scale_) / (28 * scale_));
    return row >= 1 && row < kItems ? row : -1;   // row 0 is the status line
}

void X11Tray::drawMenu() {
    if (!menu_) return;
    paintWindow(dpy_, menu_, DefaultVisual(dpy_, screen_), DefaultDepth(dpy_, screen_), menuW_, menuH_,
                [&](cairo_surface_t* s) {
        ui::CairoCanvas canvas;
        canvas.attach(s, scale_);
        const float w = menuW_ / scale_, h = menuH_ / scale_;
        canvas.beginFrame(w, h, COL_BG_CARD);
        canvas.strokeRoundRect({ 0.5f, 0.5f, w - 0.5f, h - 0.5f }, 6.0f, COL_BORDER, 1.0f);
        for (int i = 0; i < kItems; ++i) {
            const ui::Rect row{ 4.0f, 4.0f + 28.0f * i, w - 4.0f, 4.0f + 28.0f * (i + 1) };
            if (i == hover_) canvas.fillRoundRect(row, 5.0f, COL_SEC_BTN_HV);
            canvas.drawText(labels_[i], { row.left + 10.0f, row.top, row.right - 10.0f, row.bottom },
                            i == 0 ? ui::FontRole::Small : ui::FontRole::Body,
                            i == 0 ? COL_TEXT_MUTED : i == kItems - 1 ? COL_DANGER : COL_TEXT_PRIMARY,
                            ui::HAlign::Leading, ui::VAlign::Center);
        }
        canvas.endFrame();
        canvas.detach();
    });
}

X11Tray::Action X11Tray::handleEvent(const XEvent& ev, const MenuInfo& info, bool& consumed, bool& lostTray) {
    consumed = false;
    lostTray = false;
    if (ev.type == ClientMessage && ev.xclient.window == RootWindow(dpy_, screen_) &&
        ev.xclient.message_type == manager_ && static_cast<Atom>(ev.xclient.data.l[1]) == selection_) {
        consumed = true;
        if (!docked()) dock();
        return Action::Nothing;
    }
    if (ev.type == DestroyNotify && managerWin_ && ev.xdestroywindow.window == managerWin_) {
        consumed = true;
        undock();
        lostTray = true;
        return Action::Nothing;
    }
    if (icon_ && ev.xany.window == icon_) {
        consumed = true;
        if (ev.type == Expose || ev.type == ConfigureNotify || ev.type == MapNotify) drawIcon();
        if (ev.type == ButtonPress && ev.xbutton.button == Button1) return Action::Restore;
        if (ev.type == ButtonPress && ev.xbutton.button == Button3) openMenu(ev.xbutton.x_root, ev.xbutton.y_root, info);
        return Action::Nothing;
    }
    if (menu_ && ev.xany.window == menu_) {
        consumed = true;
        if (ev.type == Expose) drawMenu();
        if (ev.type == MotionNotify) {
            const bool inside = ev.xmotion.x >= 0 && ev.xmotion.x < menuW_;
            const int hover = inside ? itemAt(ev.xmotion.y) : -1;
            if (hover != hover_) {
                hover_ = hover;
                drawMenu();
            }
        }
        if (ev.type == KeyPress && XLookupKeysym(const_cast<XKeyEvent*>(&ev.xkey), 0) == XK_Escape) closeMenu();
        if (ev.type == ButtonPress) {
            const bool inside = ev.xbutton.x >= 0 && ev.xbutton.x < menuW_ && ev.xbutton.y >= 0 && ev.xbutton.y < menuH_;
            const int item = inside ? itemAt(ev.xbutton.y) : -1;
            closeMenu();
            if (item == 1) return Action::Restore;
            if (item == 2) return Action::ToggleMute;
            if (item == 3) return Action::Quit;
        }
        return Action::Nothing;
    }
    return Action::Nothing;
}

} // namespace cppdesk
