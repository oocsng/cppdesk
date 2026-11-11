#include "whiteboard_manager.hpp"
#include "../core/win_compat.hpp"

#include <cstring>
#include <algorithm>
#include <cmath>

namespace cppdesk {

namespace {

uint64_t nowMs() {
    return static_cast<uint64_t>(GetTickCount64());
}

} // namespace

WhiteboardManager::WhiteboardManager() = default;

WhiteboardManager::~WhiteboardManager() {
#if defined(_WIN32)
    if (hwndOverlay_) {
        SetWindowLongPtrW(hwndOverlay_, GWLP_USERDATA, 0);
    }
#endif
    hideHostOverlay();
}

void WhiteboardManager::startStroke(float normX, float normY) {
    std::lock_guard<std::mutex> lk(strokesMutex_);
    isDrawing_ = true;
    currentStroke_ = AnnotationStroke{};
    currentStroke_.strokeId = nextStrokeId_++;
    currentStroke_.tool = activeTool_;
    currentStroke_.argbColor = activeColor_;
    currentStroke_.thickness = activeThickness_;
    currentStroke_.timestampMs = nowMs();
    currentStroke_.points.push_back({ normX, normY });
}

void WhiteboardManager::addStrokePoint(float normX, float normY) {
    std::lock_guard<std::mutex> lk(strokesMutex_);
    if (!isDrawing_) return;
    if (!currentStroke_.points.empty()) {
        const auto& last = currentStroke_.points.back();
        float dx = normX - last.x;
        float dy = normY - last.y;
        if ((dx * dx + dy * dy) < 0.000004f) { // Ignore micro-jitter
            return;
        }
    }
    currentStroke_.points.push_back({ normX, normY });
}

AnnotationStroke WhiteboardManager::finishStroke() {
    std::lock_guard<std::mutex> lk(strokesMutex_);
    isDrawing_ = false;
    AnnotationStroke finished = currentStroke_;
    if (finished.points.size() >= 2 || (finished.points.size() == 1 && finished.tool == WhiteboardTool::LaserPointer)) {
        strokes_.push_back(finished);
        if (strokes_.size() > 250) {
            strokes_.erase(strokes_.begin());
        }
    }
    currentStroke_ = AnnotationStroke{};
    updateHostOverlayCanvas();
    return finished;
}

void WhiteboardManager::clearAllStrokes() {
    {
        std::lock_guard<std::mutex> lk(strokesMutex_);
        strokes_.clear();
        currentStroke_ = AnnotationStroke{};
        isDrawing_ = false;
    }
    updateHostOverlayCanvas();
}

void WhiteboardManager::applyRemoteStroke(const AnnotationStroke& stroke) {
    if (stroke.tool == WhiteboardTool::ClearAll) {
        clearAllStrokes();
        return;
    }
    if (stroke.tool == WhiteboardTool::LaserPointer && !stroke.points.empty()) {
        setLaserPointer(stroke.points.back().x, stroke.points.back().y);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(strokesMutex_);
        strokes_.push_back(stroke);
        if (strokes_.size() > 250) {
            strokes_.erase(strokes_.begin());
        }
    }
    updateHostOverlayCanvas();
}

void WhiteboardManager::applyRemoteClear() {
    clearAllStrokes();
}

void WhiteboardManager::setLaserPointer(float normX, float normY) {
    std::lock_guard<std::mutex> lk(laserMutex_);
    laserNormX_ = normX;
    laserNormY_ = normY;
    laserTimestampMs_ = nowMs();
}

bool WhiteboardManager::getLaserPointer(float& outX, float& outY, float& outAlpha) const {
    std::lock_guard<std::mutex> lk(laserMutex_);
    if (laserNormX_ < 0.0f || laserNormY_ < 0.0f) return false;
    uint64_t elapsed = nowMs() - laserTimestampMs_;
    if (elapsed >= 1500) {
        return false;
    }
    outX = laserNormX_;
    outY = laserNormY_;
    outAlpha = 1.0f - static_cast<float>(elapsed) / 1500.0f;
    return true;
}

std::vector<AnnotationStroke> WhiteboardManager::snapshotStrokes() const {
    std::lock_guard<std::mutex> lk(strokesMutex_);
    std::vector<AnnotationStroke> res = strokes_;
    if (isDrawing_ && !currentStroke_.points.empty()) {
        res.push_back(currentStroke_);
    }
    return res;
}

std::vector<uint8_t> WhiteboardManager::serializeStroke(const AnnotationStroke& stroke) {
    ByteWriter w;
    w.writeU32(stroke.strokeId);
    w.writeU8(static_cast<uint8_t>(stroke.tool));
    w.writeU32(stroke.argbColor);
    w.writeF32(stroke.thickness);
    uint16_t ptCount = static_cast<uint16_t>(std::min<size_t>(stroke.points.size(), 65535));
    w.writeU16(ptCount);
    for (size_t i = 0; i < ptCount; ++i) {
        w.writeF32(stroke.points[i].x);
        w.writeF32(stroke.points[i].y);
    }
    return w.buffer();
}

bool WhiteboardManager::deserializeStroke(const uint8_t* data, size_t len, AnnotationStroke& outStroke) {
    if (!data || len < (4 + 1 + 4 + 4 + 2)) return false;
    try {
        ByteReader r(data, len);
        outStroke.strokeId = r.readU32();
        uint8_t toolVal = r.readU8();
        outStroke.tool = (toolVal <= 4) ? static_cast<WhiteboardTool>(toolVal) : WhiteboardTool::Pen;
        outStroke.argbColor = r.readU32();
        float thick = r.readF32();
        outStroke.thickness = (std::isnan(thick) || std::isinf(thick)) ? 3.5f : std::clamp(thick, 0.5f, 50.0f);
        uint16_t ptCount = r.readU16();
        ptCount = std::min<uint16_t>(ptCount, 4096);
        outStroke.points.clear();
        outStroke.points.reserve(ptCount);
        for (uint16_t i = 0; i < ptCount; ++i) {
            if (!r.hasRemaining(8)) break;
            float px = r.readF32();
            float py = r.readF32();
            if (std::isnan(px) || std::isinf(px)) px = 0.0f;
            if (std::isnan(py) || std::isinf(py)) py = 0.0f;
            outStroke.points.push_back({ std::clamp(px, 0.0f, 1.0f), std::clamp(py, 0.0f, 1.0f) });
        }
        outStroke.timestampMs = nowMs();
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<uint8_t> WhiteboardManager::serializeClearPacket() {
    AnnotationStroke clearStroke{};
    clearStroke.tool = WhiteboardTool::ClearAll;
    return serializeStroke(clearStroke);
}

// ---------------- Host Click-Through Layered Window ----------------

#if !defined(_WIN32)
void WhiteboardManager::showHostOverlay() {}
void WhiteboardManager::hideHostOverlay() {}
void WhiteboardManager::updateHostOverlayCanvas() {}
#else

LRESULT CALLBACK WhiteboardManager::OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        // Fill background with black color key (which is rendered completely transparent by Windows)
        HBRUSH bgBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(hdc, &rc, bgBrush);
        DeleteObject(bgBrush);

        auto* mgr = reinterpret_cast<WhiteboardManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (mgr) {
            auto strokes = mgr->snapshotStrokes();
            int screenW = rc.right - rc.left;
            int screenH = rc.bottom - rc.top;

            for (const auto& s : strokes) {
                if (s.points.size() < 2) continue;

                COLORREF gdiColor = RGB(
                    (s.argbColor >> 16) & 0xFF,
                    (s.argbColor >> 8) & 0xFF,
                    s.argbColor & 0xFF
                );
                int penWidth = static_cast<int>(std::clamp(s.thickness, 1.0f, 20.0f));
                HPEN pen = CreatePen(PS_SOLID, penWidth, gdiColor);
                HGDIOBJ oldPen = SelectObject(hdc, pen);

                std::vector<POINT> pts;
                pts.reserve(s.points.size());
                for (const auto& pt : s.points) {
                    pts.push_back({
                        static_cast<LONG>(pt.x * screenW),
                        static_cast<LONG>(pt.y * screenH)
                    });
                }
                Polyline(hdc, pts.data(), static_cast<int>(pts.size()));

                // Arrow head rendering for Arrow tool
                if (s.tool == WhiteboardTool::Arrow && pts.size() >= 2) {
                    POINT p1 = pts[pts.size() - 2];
                    POINT p2 = pts[pts.size() - 1];
                    float dx = static_cast<float>(p2.x - p1.x);
                    float dy = static_cast<float>(p2.y - p1.y);
                    float len = std::sqrt(dx * dx + dy * dy);
                    if (len > 4.0f) {
                        float udx = dx / len;
                        float udy = dy / len;
                        float arrowSz = 16.0f + penWidth * 1.5f;
                        POINT tip = p2;
                        POINT wing1 = {
                            static_cast<LONG>(tip.x - arrowSz * udx + (arrowSz * 0.5f) * -udy),
                            static_cast<LONG>(tip.y - arrowSz * udy + (arrowSz * 0.5f) * udx)
                        };
                        POINT wing2 = {
                            static_cast<LONG>(tip.x - arrowSz * udx - (arrowSz * 0.5f) * -udy),
                            static_cast<LONG>(tip.y - arrowSz * udy - (arrowSz * 0.5f) * udx)
                        };
                        POINT arrowPoly[3] = { tip, wing1, wing2 };
                        HBRUSH brush = CreateSolidBrush(gdiColor);
                        HGDIOBJ oldBrush = SelectObject(hdc, brush);
                        Polygon(hdc, arrowPoly, 3);
                        SelectObject(hdc, oldBrush);
                        DeleteObject(brush);
                    }
                }

                SelectObject(hdc, oldPen);
                DeleteObject(pen);
            }

            // Laser pointer dot
            float lx = 0.0f, ly = 0.0f, lAlpha = 0.0f;
            if (mgr->getLaserPointer(lx, ly, lAlpha) && lAlpha > 0.05f) {
                int cx = static_cast<int>(lx * screenW);
                int cy = static_cast<int>(ly * screenH);
                int rad = 8;
                HBRUSH lBrush = CreateSolidBrush(RGB(255, 45, 85));
                HGDIOBJ oldB = SelectObject(hdc, lBrush);
                HPEN lPen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                HGDIOBJ oldP = SelectObject(hdc, lPen);
                Ellipse(hdc, cx - rad, cy - rad, cx + rad, cy + rad);
                SelectObject(hdc, oldP);
                SelectObject(hdc, oldB);
                DeleteObject(lPen);
                DeleteObject(lBrush);
            }
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void WhiteboardManager::showHostOverlay() {
    if (hwndOverlay_) return;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CppDesk_Whiteboard_Overlay";
    RegisterClassExW(&wc);

    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    hwndOverlay_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        L"CppDesk Whiteboard Overlay",
        WS_POPUP | WS_VISIBLE,
        vx, vy, vw, vh,
        nullptr, nullptr, hInst, nullptr
    );

    if (hwndOverlay_) {
        SetWindowLongPtrW(hwndOverlay_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        // Use RGB(0,0,0) as transparent color key
        SetLayeredWindowAttributes(hwndOverlay_, RGB(0, 0, 0), 255, LWA_COLORKEY);
        ShowWindow(hwndOverlay_, SW_SHOWNOACTIVATE);
        UpdateWindow(hwndOverlay_);
    }
}

void WhiteboardManager::hideHostOverlay() {
    if (hwndOverlay_) {
        DestroyWindow(hwndOverlay_);
        hwndOverlay_ = nullptr;
    }
}

void WhiteboardManager::updateHostOverlayCanvas() {
    if (hwndOverlay_) {
        InvalidateRect(hwndOverlay_, nullptr, FALSE);
    }
}
#endif

} // namespace cppdesk
