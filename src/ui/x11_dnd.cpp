#include "ui_platform.hpp"   // before the X headers, whose Success macro breaks protocol.hpp

#include "x11_dnd.hpp"

#include <X11/Xatom.h>

namespace cppdesk {

XdndTarget::XdndTarget(Display* dpy, Window win) : dpy_(dpy), win_(win) {
    enter_ = XInternAtom(dpy_, "XdndEnter", False);
    position_ = XInternAtom(dpy_, "XdndPosition", False);
    status_ = XInternAtom(dpy_, "XdndStatus", False);
    leave_ = XInternAtom(dpy_, "XdndLeave", False);
    drop_ = XInternAtom(dpy_, "XdndDrop", False);
    finished_ = XInternAtom(dpy_, "XdndFinished", False);
    selection_ = XInternAtom(dpy_, "XdndSelection", False);
    uriList_ = XInternAtom(dpy_, "text/uri-list", False);
    actionCopy_ = XInternAtom(dpy_, "XdndActionCopy", False);
    typeList_ = XInternAtom(dpy_, "XdndTypeList", False);
    prop_ = XInternAtom(dpy_, "CPPDESK_DROP", False);
    const Atom version = 5;
    XChangeProperty(dpy_, win_, XInternAtom(dpy_, "XdndAware", False), XA_ATOM, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&version), 1);
}

void XdndTarget::sendToSource(Atom type, long l1, long l2) {
    if (!source_) return;
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = source_;
    ev.xclient.message_type = type;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = static_cast<long>(win_);
    ev.xclient.data.l[1] = l1;
    ev.xclient.data.l[2] = l2;
    XSendEvent(dpy_, source_, False, NoEventMask, &ev);
    XFlush(dpy_);
}

bool XdndTarget::handleClientMessage(const XEvent& ev) {
    if (ev.type != ClientMessage) return false;
    const Atom type = ev.xclient.message_type;
    const long* l = ev.xclient.data.l;
    if (type == enter_) {
        source_ = static_cast<Window>(l[0]);
        offersUris_ = false;
        if (l[1] & 1) {   // more than three types: they are listed on the source window
            Atom actual = 0;
            int format = 0;
            unsigned long count = 0, after = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(dpy_, source_, typeList_, 0, 1024, False, XA_ATOM, &actual, &format, &count, &after,
                                   &data) == Success && data) {
                const Atom* types = reinterpret_cast<const Atom*>(data);
                for (unsigned long i = 0; i < count; ++i) offersUris_ = offersUris_ || types[i] == uriList_;
            }
            if (data) XFree(data);
        } else {
            for (int i = 2; i <= 4; ++i) offersUris_ = offersUris_ || static_cast<Atom>(l[i]) == uriList_;
        }
        return true;
    }
    if (type == position_) {
        source_ = static_cast<Window>(l[0]);
        Window child = 0;
        XTranslateCoordinates(dpy_, DefaultRootWindow(dpy_), win_, static_cast<int>((l[2] >> 16) & 0xFFFF),
                              static_cast<int>(l[2] & 0xFFFF), &x_, &y_, &child);
        XEvent ev2{};
        ev2.xclient.type = ClientMessage;
        ev2.xclient.window = source_;
        ev2.xclient.message_type = status_;
        ev2.xclient.format = 32;
        ev2.xclient.data.l[0] = static_cast<long>(win_);
        ev2.xclient.data.l[1] = offersUris_ ? 1 : 0;   // accept, no rectangle to stay quiet in
        ev2.xclient.data.l[4] = offersUris_ ? static_cast<long>(actionCopy_) : 0;
        XSendEvent(dpy_, source_, False, NoEventMask, &ev2);
        XFlush(dpy_);
        return true;
    }
    if (type == leave_) {
        source_ = 0;
        offersUris_ = false;
        return true;
    }
    if (type == drop_) {
        source_ = static_cast<Window>(l[0]);
        if (offersUris_) {
            XConvertSelection(dpy_, selection_, uriList_, prop_, win_, static_cast<Time>(l[2]));
            XFlush(dpy_);
        } else {
            sendToSource(finished_, 0, 0);
            source_ = 0;
        }
        return true;
    }
    return false;
}

std::optional<XdndTarget::DropResult> XdndTarget::handleSelectionNotify(const XEvent& ev) {
    if (ev.type != SelectionNotify || ev.xselection.selection != selection_) return std::nullopt;
    DropResult result;
    result.x = x_;
    result.y = y_;
    if (ev.xselection.property != None) {
        Atom actual = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        if (XGetWindowProperty(dpy_, win_, prop_, 0, 1 << 22, True, AnyPropertyType, &actual, &format, &count, &after,
                               &data) == Success && data && format == 8) {
            result.paths = UiPlatform::parseFileUriList(std::string(reinterpret_cast<char*>(data), count));
        }
        if (data) XFree(data);
    }
    const bool ok = !result.paths.empty();
    sendToSource(finished_, ok ? 1 : 0, ok ? static_cast<long>(actionCopy_) : 0);
    source_ = 0;
    offersUris_ = false;
    if (!ok) return std::nullopt;
    return result;
}

} // namespace cppdesk
