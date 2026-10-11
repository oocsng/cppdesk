#pragma once

#include "../core/protocol.hpp"
#include "../core/crypto_identity.hpp"
#include "../net/network_engine.hpp"
#include "../media/session_recorder.hpp"
#include "../media/session_recording_player.hpp"
#include "../control/session_tab_manager.hpp"
#include "../control/shortcut_manager.hpp"
#include "../core/qr_matrix.hpp"
#include "../capture/session_rewind_buffer.hpp"
#include "../capture/privacy_cloak_manager.hpp"
#include "notification_manager.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <functional>
#include "../net/updater.hpp"

namespace cppdesk {

static constexpr UINT WM_DESK_UPDATE_CHECK_DONE = WM_USER + 102;
static constexpr UINT WM_APP_FRAME_READY        = WM_APP + 101;

enum class ActiveTab : uint8_t {
    Dashboard     = 0,
    RemoteSession = 1,
    Settings      = 2
};

enum class SettingsSection : uint8_t {
    Appearance      = 0,
    DisplayQuality  = 1,
    Permissions     = 2,
    TwoFactorAuth   = 3,
    NetworkRelay    = 4,
    CurtainPrivacy  = 5,
    SystemService   = 6,
    About           = 7
};

enum class FocusedField : uint8_t {
    None            = 0,
    RemoteId        = 1,
    RemotePassword  = 2,
    LocalPassword   = 3,
    RelayServer     = 4,
    RemoteCanvas    = 5,
    ChatInput       = 6,
    TerminalInput   = 7,
    ForwardLocal    = 8,
    ForwardTarget   = 9,
    ForwardDesc     = 10,
    DashboardSearch = 11,
    EditAlias       = 12,
    EditTag         = 13,
    EditNotes       = 14,
    RelayAuthKey    = 15,
    StunServer      = 16,
    ClipboardSearch = 17,
    PrivacyBrand    = 18,
    PrivacyNotice   = 19,
    TotpEnrollTestCode = 20,
    TotpModalCode      = 21,
    SyncLocalPath      = 22,
    SyncRemotePath     = 23,
    ScratchpadText     = 24,
    OmnibarInput       = 25,
    PrivacyKeywords    = 26
};

enum class DrawerTab : uint8_t {
    FilesAndClip     = 0,
    LiveChat         = 1,
    RemoteTerminal   = 2,
    Diagnostics      = 3,
    ClipboardHistory = 4,
    Sync             = 5,
    Notes            = 6
};

struct UiRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    float centerX() const { return (left + right) * 0.5f; }
    float centerY() const { return (top + bottom) * 0.5f; }

    bool contains(float x, float y) const {
        return x >= left && x <= right && y >= top && y <= bottom;
    }

    UiRect offset(float dx, float dy) const {
        return { left + dx, top + dy, right + dx, bottom + dy };
    }

    UiRect inflate(float dx, float dy) const {
        return { left - dx, top - dy, right + dx, bottom + dy };
    }
};

struct ClickRegion {
    UiRect                rect;
    std::string           id;
    std::function<void()> onClick;
    bool                  isTextInput = false;
};

struct WidgetAnimState {
    float hoverT    = 0.0f;
    float hoverVel  = 0.0f;
    float pressT    = 0.0f;
    float pressVel  = 0.0f;
    float focusT    = 0.0f;
    float focusVel  = 0.0f;
    float toggleT   = 0.0f;
    float toggleVel = 0.0f;
    float rippleT   = 1.0f;
    float rippleX   = 0.0f;
    float rippleY   = 0.0f;
    bool  toggleInitialized = false;
};

class CppDeskWindow {
public:
    CppDeskWindow(IdentityManager& identity, NetworkEngine& network, const std::string& autoConnectTarget = "");
    ~CppDeskWindow();

    bool create(HINSTANCE hInstance, int nCmdShow);
    int messageLoop();

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    // Direct2D / DirectWrite lifecycle
    bool initGraphics();
    void discardDeviceResources();
    void releaseGraphics();
    void applyWindowThemeAttribute();
    void switchTab(ActiveTab newTab);
    void onPaint();

    // Second-order macOS spring animation step (returns true if any spring is in motion)
    bool stepAnimations(float dt);

    // Rendering views
    void drawTopNavBar(float width, float& outTopOffset);
    void drawDashboardView(const UiRect& bounds, float alpha = 1.0f);
    void drawSessionTabBar(const UiRect& bounds, float alpha = 1.0f);
    void drawRemoteSessionView(const UiRect& bounds, float alpha = 1.0f);
    void drawSettingsView(const UiRect& bounds, float alpha = 1.0f);
    void drawFileTransferDrawer(const UiRect& bounds, float slideProgress);
    void drawSyncDrawer(const UiRect& contentRect);
    void drawIncomingApprovalModal(float width, float height, float modalProgress);
    void drawDynamicIslandToolbar(float width, float height);
    void drawClipboardTransferPill(float width, float height);
    void drawShortcutsModal(float width, float height, float modalProgress);
    void drawPortForwardModal(float width, float height, float modalProgress);
    void drawAddressBookModal(float width, float height, float modalProgress);
    void drawWhiteboardOverlay(const UiRect& canvasRect);
    void drawPerformanceHud(const UiRect& stageRect, float alpha);
    void drawUpdateRequiredModal(float width, float height, float modalProgress);
    void drawRebootConfirmModal(float width, float height, float modalProgress);
    void drawHwAccelRestartModal(float width, float height, float modalProgress);
    void drawAudioVolumePopup(float anchorX, float anchorY);
    void drawRecordingPlayerModal(float width, float height, float modalProgress);
    void drawToastBanner(float width, float height, float toastProgress);
    void drawRewindScrubberBar(const UiRect& stage);
    void drawOmnibarModal(float width, float height, float modalProgress);
    void triggerUpdateCheck(bool manual);

    // Primitive drawing & vector icon helpers
    void drawCardShadow(const UiRect& r, float radius, float intensity = 1.0f);
    void drawCardSurface(const UiRect& r, float radius, float alpha, bool accentHeader = false);
    void drawPulseDot(float cx, float cy, float baseRadius, D2D1_COLOR_F color, float alpha = 1.0f);
    void drawSparkline(const UiRect& r, const float* values, size_t count, float maxVal, D2D1_COLOR_F color, float alpha = 1.0f);
    void drawIconStar(float cx, float cy, float radius, bool filled, D2D1_COLOR_F color);
    void drawIconClose(float cx, float cy, float halfSize, D2D1_COLOR_F color, float strokeWidth = 1.6f);
    void drawIconTheme(float cx, float cy, float radius, bool isDark, D2D1_COLOR_F color);
    void fillRoundRect(const UiRect& r, float radius, D2D1_COLOR_F color);
    void strokeRoundRect(const UiRect& r, float radius, D2D1_COLOR_F color, float strokeWidth = 1.0f);
    void drawText(const std::string& utf8, const UiRect& r, IDWriteTextFormat* fmt, D2D1_COLOR_F color,
                  DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    void drawButton(const std::string& id, const UiRect& r, const std::string& label,
                    D2D1_COLOR_F bgColor, D2D1_COLOR_F hoverColor, D2D1_COLOR_F textColor,
                    float radius, std::function<void()> onClick, IDWriteTextFormat* fmt = nullptr,
                    bool hasBorder = false, D2D1_COLOR_F borderColor = D2D1::ColorF(0, 0, 0, 0),
                    D2D1_COLOR_F hoverTextColor = D2D1::ColorF(0, 0, 0, -1.0f));
    void drawTextField(const std::string& id, FocusedField fieldType, const UiRect& r,
                       const std::string& value, const std::string& placeholder, bool maskPassword);
    void drawToggleSwitch(const std::string& id, const UiRect& r, bool checked,
                          const std::string& label, std::function<void()> onToggle);
    void ensureAppIconBitmap();

    // Two-Factor Authentication (TOTP) rendering
    void renderQrCodeDirect2D(
        const UiRect& rect,
        const std::vector<uint8_t>& matrix,
        int matrixSize,
        float alpha = 1.0f);
    void updateTotpQrMatrix();
    void drawTotpChallengeModal(float clientW, float clientH);

    // Input & interaction handlers
    void onMouseMove(float x, float y);
    void onMouseButton(MouseButtonId btn, bool isDown, float x, float y);
    void onMouseWheel(int delta);
    void onCharInput(wchar_t ch);
    void onKeyEvent(uint16_t vk, uint16_t scan, bool isDown, bool isExtended);
    void onDropFiles(HDROP hDrop);

    // Actions
    void initiateConnection();
    void openSendFileDialog();
    void openPickFolderDialog(std::string& outPath);
    void saveRemoteScreenshot();
    void toggleScreenRecording();
    void sendChatFromInput();
    void sendTerminalFromInput();
    void toggleFullscreen();
    void toggleShortcutsModal();
    void openRecordingPlayer(const std::string& filePath = "");
    void closeRecordingPlayer();
    void switchToSessionTab(uint32_t tabId, bool force = false);
    void closeSessionTab(uint32_t tabId);
    void showToast(const std::string& message, bool isError = false);
    void restoreFromTray(NotificationType contextType = NotificationType::GeneralInfo);
    bool mapCanvasPointToNormalized(float x, float y, float& outNormX, float& outNormY) const;
    std::string* activeFocusedTextBuffer();

    IdentityManager&        identity_;
    NetworkEngine&          network_;
    HWND                    hwnd_ = nullptr;
    std::unique_ptr<NotificationManager> notificationMgr_;

    // Direct2D & DirectWrite resources
    ID2D1Factory*           d2dFactory_ = nullptr;
    ID2D1HwndRenderTarget*  renderTarget_ = nullptr;
    ID2D1SolidColorBrush*   solidBrush_ = nullptr;
    ID2D1Bitmap*            remoteBitmap_ = nullptr;
    ID2D1Bitmap*            appIconBitmap_ = nullptr;
    ID2D1PathGeometry*      geoUnitStar_ = nullptr;
    int                     bitmapW_ = 0;
    int                     bitmapH_ = 0;

    IDWriteFactory*         dwriteFactory_ = nullptr;
    IDWriteTextFormat*      fmtHeroId_ = nullptr;
    IDWriteTextFormat*      fmtHeading_ = nullptr;
    IDWriteTextFormat*      fmtSubheading_ = nullptr;
    IDWriteTextFormat*      fmtBody_ = nullptr;
    IDWriteTextFormat*      fmtBodyBold_ = nullptr;
    IDWriteTextFormat*      fmtSmall_ = nullptr;
    IDWriteTextFormat*      fmtMono_ = nullptr;

    // Interactive UI state
    ActiveTab               activeTab_ = ActiveTab::Dashboard;
    DrawerTab               drawerTab_ = DrawerTab::FilesAndClip;
    ScaleMode               scaleMode_ = ScaleMode::FitAspect;
    FocusedField            focusedField_ = FocusedField::RemoteId;
    bool                    showFileDrawer_ = false;
    bool                    remoteInputEnabled_ = true;
    bool                    showLocalPassword_ = false;
    bool                    showRemotePassword_ = false;
    bool                    isFullscreen_ = false;
    WINDOWPLACEMENT         savedWindowPlacement_{};

    std::string             autoConnectTarget_;
    std::string             remoteIdInput_;
    std::string             remotePasswordInput_;
    std::string             localPasswordEdit_;
    std::string             relayServerEdit_;
    std::string             relayAuthKeyEdit_;
    std::string             stunServerEdit_;
    uint8_t                 relayModeEdit_ = 0;
    std::string             privacyBrandEdit_;
    std::string             privacyNoticeEdit_;
    bool                    relayDiagnosticRunning_ = false;
    std::string             relayDiagnosticStatus_;
    int                     relayDiagnosticLatency_ = -1;
    bool                    relayDiagnosticSuccess_ = false;
    std::string             chatInput_;
    int32_t                 chatScrollOffset_ = 0;
    std::string             clipSearchQuery_;
    int32_t                 clipHistoryScrollOffset_ = 0;

    // Pending incoming request local permission checkboxes
    uint8_t                 modalPermissions_ = PERM_ALL;
    bool                    modalWasActive_ = false;
    uint32_t                lastSeenUnreadChat_ = 0;
    bool                    prevIncomingPending_ = false;
    size_t                  lastNotifiedChatCount_ = 0;
    size_t                  prevActiveTransfers_ = 0;
    ViewerConnectionState   prevNotifiedViewerState_ = ViewerConnectionState::Disconnected;

    // Remote video canvas cache
    uint64_t                displayedFrameSeq_ = 0;
    std::vector<uint8_t>    frameBufferBgra_;
    int                     frameBufferW_ = 0;
    int                     frameBufferH_ = 0;
    CursorState             remoteCursor_{};
    UiRect                  renderedCanvasRect_{};
    UiRect                  stageRect_{};

    // Rolling network telemetry history
    static constexpr size_t SPARKLINE_SAMPLES = 36;
    std::array<float, SPARKLINE_SAMPLES> rttHistory_{};
    std::array<float, SPARKLINE_SAMPLES> fpsHistory_{};
    uint64_t                lastTelemetrySampleTick_ = 0;

    // Hit-test regions & macOS Spring Physics Animation Engine state
    std::vector<ClickRegion>                        clickRegions_;
    std::unordered_map<std::string, WidgetAnimState> widgetAnims_;
    std::string             hoveredWidgetId_;
    std::string             pressedWidgetId_;
    bool                    hoveredIsTextInput_ = false;
    float                   mouseX_ = -1000.0f;
    float                   mouseY_ = -1000.0f;
    bool                    mouseInsideClient_ = false;
    bool                    trackingMouseLeave_ = false;
    bool                    mouseLeftDown_ = false;
    uint64_t                lastMouseSendTick_ = 0;

    // High-precision timing & second-order spring states (pos + velocity)
    int64_t                 qpcFreq_ = 0;
    int64_t                 lastQpcCounter_ = 0;
    float                   lastDt_ = 0.016f;
    bool                    inlineAnimActive_ = false;
    float                   animTimeSec_ = 0.0f;

    float                   viewportPos_ = 0.0f;
    float                   viewportVel_ = 0.0f;
    float                   dashViewAnimT_ = 1.0f;
    float                   sessViewAnimT_ = 0.0f;
    float                   settingsViewAnimT_ = 0.0f;
    float                   tabEnterStaggerT_ = 1.0f;
    float                   tabEnterStaggerVel_ = 0.0f;

    // Liquid macOS segmented control pill (independent left/right edge springs)
    float                   navPillLeft_ = 400.0f;
    float                   navPillRight_ = 510.0f;
    float                   navPillVelL_ = 0.0f;
    float                   navPillVelR_ = 0.0f;
    float                   targetPillLeft_ = 400.0f;
    float                   targetPillRight_ = 510.0f;
    bool                    navPillInit_ = false;

    float                   themeAnimT_ = 0.0f;
    float                   themeAnimVel_ = 0.0f;
    float                   drawerAnimT_ = 0.0f;
    float                   drawerAnimVel_ = 0.0f;
    float                   modalAnimT_ = 0.0f;
    float                   modalAnimVel_ = 0.0f;
    float                   toastAnimT_ = 0.0f;
    float                   toastAnimVel_ = 0.0f;
    float                   transferProgSmooth_[16]{};

    // Toast notification
    std::string             toastText_;
    bool                    toastIsError_ = false;
    uint64_t                toastExpireTick_ = 0;

    // Fullscreen Dynamic Island Toolbar state
    float                   floatingToolbarY_ = -64.0f;
    float                   floatingToolbarVel_ = 0.0f;
    bool                    floatingToolbarPinned_ = false;
    bool                    showDisplayMenu_ = false;
    bool                    showAdminMenu_ = false;
    bool                    showQualityMenu_ = false;
    bool                    lastClipTransferActive_ = false;
    uint64_t                lastClipTransferBytes_ = 0;

    // Keyboard Shortcuts Sheet Modal state
    bool                    showShortcutsModal_ = false;
    float                   shortcutsModalAnimT_ = 0.0f;
    float                   shortcutsModalAnimVel_ = 0.0f;

    // Real-Time Performance & Diagnostics HUD Overlay state
    bool                    showPerformanceHud_ = false;
    float                   hudAnimT_ = 0.0f;
    float                   hudAnimVel_ = 0.0f;

    // Master Volume Popup state
    bool                    showAudioVolumePopup_ = false;

    // Remote Terminal UI state (v2.1.0)
    std::string             terminalInputText_;
    int                     terminalHistoryIndex_ = -1;
    float                   terminalScrollOffset_ = 0.0f;

    // Remote Hardware Diagnostics & Process Telemetry UI state (Feature 1)
    float                   diagnosticsScrollOffset_ = 0.0f;

    // Settings Tab vertical scroll & section state
    SettingsSection         settingsSection_ = SettingsSection::Appearance;
    float                   settingsScrollOffset_ = 0.0f;
    float                   settingsScrollTarget_ = 0.0f;
    float                   settingsScrollVel_ = 0.0f;
    float                   settingsMaxScroll_ = 0.0f;
    bool                    draggingSettingsScrollbar_ = false;
    float                   settingsScrollbarGrabOffset_ = 0.0f;
    UiRect                  settingsScrollTrackRect_{};
    UiRect                  settingsScrollThumbRect_{};

    // Horizontal HUD and Drawer Tabs smooth scroll state
    float                   sessionHudScrollOffset_ = 0.0f;
    float                   sessionHudScrollTarget_ = 0.0f;
    float                   sessionHudScrollVel_    = 0.0f;
    float                   sessionHudMaxScroll_    = 0.0f;
    UiRect                  sessionHudBarRect_{};
    float                   drawerTabsScrollOffset_ = 0.0f;
    float                   drawerTabsScrollTarget_ = 0.0f;
    float                   drawerTabsScrollVel_    = 0.0f;
    float                   drawerTabsMaxScroll_    = 0.0f;

    // Direct Canvas Drag-and-Drop File Drop UI state (Feature 2)
    bool                    canvasDropEffectActive_ = false;
    D2D1_POINT_2F           canvasDropPos_{ 0.0f, 0.0f };
    uint64_t                canvasDropTick_ = 0;
    int                     canvasDropCount_ = 0;

    // In-Session Screen Recording Subsystem (Feature 3)
    SessionRecorder         sessionRecorder_;

    // In-Session Session Recording Player & Transcoder
    bool                                    showRecordingPlayer_ = false;
    float                                   recordingPlayerModalAnimT_ = 0.0f;
    float                                   recordingPlayerModalAnimVel_ = 0.0f;
    std::unique_ptr<SessionRecordingPlayer> recordingPlayer_;
    ID2D1Bitmap*                            recordingPlayerBitmap_ = nullptr;
    int                                     recordingPlayerBitmapW_ = 0;
    int                                     recordingPlayerBitmapH_ = 0;
    bool                                    draggingPlayerScrubber_ = false;
    uint32_t                                trimInFrame_ = 0;
    uint32_t                                trimOutFrame_ = 0;
    bool                                    hasTrimIn_ = false;
    bool                                    hasTrimOut_ = false;
    UiRect                                  playerScrubberTrackRect_{};

    // TCP Port Forwarding modal state (v2.1.0)
    bool                    showPortForwardModal_ = false;
    float                   portForwardModalAnimT_ = 0.0f;
    float                   portForwardModalAnimVel_ = 0.0f;
    std::string             forwardLocalPortEdit_ = "33890";
    std::string             forwardTargetPortEdit_ = "3389";
    std::string             forwardDescEdit_ = "RDP Remote Desktop";

    // Dashboard Address Book search & tag filtering (v2.1.0)
    std::string             dashboardSearchQuery_;
    std::string             dashboardFilterTag_ = "All";
    bool                    showAddressBookEditModal_ = false;
    float                   addressBookModalAnimT_ = 0.0f;
    float                   addressBookModalAnimVel_ = 0.0f;
    uint64_t                editingDeskId_ = 0;
    std::string             editAliasInput_;
    std::string             editTagInput_;
    std::string             editNotesInput_;

    // Whiteboard & Screen Annotation UI state (v2.1.0)
    bool                    whiteboardActive_ = false;
    WhiteboardTool          whiteboardTool_ = WhiteboardTool::Pen;
    uint32_t                whiteboardColor_ = 0xFFFF3B30; // Red
    float                   whiteboardThickness_ = 3.5f;

    ViewerConnectionState   prevViewerState_ = ViewerConnectionState::Disconnected;
    bool                    prevHostActive_ = false;
    std::string             prevViewerHostname_;
    std::string             prevHostClientName_;
    size_t                  prevCompletedTransfersCount_ = 0;
    bool                    prevHasPendingIncoming_ = false;

    // Mandatory Auto-Updater state (v3.0.0)
    bool                    showUpdateRequiredModal_ = false;
    float                   updateModalAnimT_ = 0.0f;
    float                   updateModalAnimVel_ = 0.0f;
    bool                    isCheckingUpdates_ = false;
    std::string             updateStatusText_ = "Version v3.4.1 (Up to date)";
    UpdateInfo              latestUpdateInfo_{};
    float                   startupUpdateCheckTimer_ = 2.0f;
    bool                    startupCheckTriggered_ = false;

    // In-App Self-Updater Download State
    bool                    isDownloadingUpdate_ = false;
    float                   updateDownloadProgress_ = 0.0f;
    uint64_t                updateDownloadedBytes_ = 0;
    uint64_t                updateTotalBytes_ = 0;
    double                  updateSpeedBps_ = 0.0;
    std::string             updateStatusMsg_;
    std::string             peerMismatchHostVer_;
    std::string             peerMismatchViewerVer_;
    bool                    showPeerMismatchModal_ = false;

    // Remote Reboot & Reconnect Modal (v3.2.0)
    bool                    showRebootConfirmModal_ = false;
    bool                    rebootSafeModeChoice_ = false;
    float                   rebootModalAnimT_ = 0.0f;
    float                   rebootModalAnimVel_ = 0.0f;

    // Hardware Acceleration Restart Prompt Modal
    bool                    showHwAccelRestartModal_ = false;
    bool                    pendingHwAccelChoice_ = true;
    float                   hwAccelModalAnimT_ = 0.0f;
    float                   hwAccelModalAnimVel_ = 0.0f;

    // Multi-Session Tabbed Management (Feature 6)
    SessionTabManager       sessionTabs_;

    // Event-driven 60 FPS viewer frame presentation
    std::atomic<bool>       frameRedrawPending_{false};

    // Mouse Motion Coalescing & Tail Preservation
    void                    flushPendingMouseMove();
    float                   coalescedMouseNormX_ = 0.0f;
    float                   coalescedMouseNormY_ = 0.0f;
    bool                    hasPendingMouseMove_ = false;

    // Two-Factor Authentication (TOTP) UI state
    std::string             totpEnrollCodeEdit_;
    std::string             totpEnrollFeedback_;
    bool                    totpEnrollSuccess_ = false;
    std::vector<uint8_t>    totpQrMatrix_;
    int                     totpQrMatrixSize_ = 0;
    bool                    showTotpRegenConfirm_ = false;

    // Viewer TOTP Challenge Modal state
    bool                    showTotpModal_ = false;
    float                   totpModalAnimT_ = 0.0f;
    float                   totpModalAnimVel_ = 0.0f;
    std::string             totpModalInput_;
    std::string             totpModalError_;

    // Remote File Synchronization UI State
    std::string             syncLocalPathEdit_;
    std::string             syncRemotePathEdit_;
    SyncMode                syncSelectedMode_ = SyncMode::PushMirror;
    float                   syncDiffScrollOffset_ = 0.0f;
    bool                    syncMirrorPurgeChoice_ = false;

    // 60-Second In-Memory Session Rewind Scrubber
    SessionRewindBuffer     rewindBuffer_;
    bool                    showRewindScrubber_ = false;
    int64_t                 rewindOffsetMs_ = 0;
    bool                    rewindFrozen_ = false;
    std::vector<uint8_t>    rewindCachedFrameBgra_;
    int                     rewindCachedW_ = 0;
    int                     rewindCachedH_ = 0;
    uint64_t                lastRewindPushTickMs_ = 0;

    // Dual-Peer Collaborative Scratchpad UI
    std::string             scratchpadTextEdit_;
    uint64_t                lastSeenScratchpadRev_ = 0;
    float                   scratchpadScrollOffset_ = 0.0f;

    // Spotlight Omnibar Command Palette UI
    bool                    showOmnibarModal_ = false;
    float                   omnibarModalAnimT_ = 0.0f;
    float                   omnibarModalAnimVel_ = 0.0f;
    std::string             omnibarQuery_;
    int                     omnibarSelectedIndex_ = 0;
    std::vector<OmnibarCommand> omnibarFiltered_;
    std::string             omnibarLastOutput_;
    int32_t                 omnibarLastExitCode_ = 0;
    bool                    omnibarHasOutput_ = false;

    // Privacy Cloak Settings UI
    std::string             privacyKeywordsEdit_;
};

} // namespace cppdesk
