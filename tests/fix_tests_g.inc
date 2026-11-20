// Regression tests for full-review fix worker G. Only worker G edits this file.

namespace {
class FixGShell : public FakeShell {
public:
    mutable int serviceStateCalls = 0;
    ui::ServiceState svcState = ui::ServiceState::Pending;
    bool shiftDown = false;
    bool ctrlDown = false;
    std::function<void(bool, const std::string&)> capturedDownloadDone;

    ui::ServiceState serviceState() const override {
        ++serviceStateCalls;
        return svcState;
    }
    bool isKeyDown(uint16_t vk) const override {
        if (vk == VK_SHIFT) return shiftDown;
        if (vk == VK_CONTROL) return ctrlDown;
        return false;
    }
    void startUpdateDownload(const std::string&, uint64_t, const std::string&,
                             std::function<void(bool, const std::string&)> done) override {
        capturedDownloadDone = std::move(done);
    }
};
} // namespace

void testFixesG() {
    std::cout << "[FIX-G] Running AppUi regression tests..." << std::endl;

    // #3: Privacy Cloak settings propagate to NetworkEngine and PrivacyCloakManager
    {
        IdentityManager id(4901);
        id.loadOrCreate();
        AppSettings s = id.settings();
        s.privacyCloakEnabled = true;
        s.privacyCloakStyle = 1;
        s.privacyCloakKeywords = "Payroll,TopSecret";
        id.updateSettings(s);

        NetworkEngine net(id);
        FixGShell shell;
        AppUi app(id, net, shell, "");

        PrivacyCloakManager cloak;
        net.applyPrivacyCloakConfig(cloak);
        TEST_ASSERT(cloak.isEnabled() == true);
        TEST_ASSERT(cloak.redactionStyle() == 1);
        TEST_ASSERT(cloak.matchesSensitiveTitle(L"Q4 Payroll Sheet"));
        TEST_ASSERT(cloak.matchesSensitiveTitle(L"TopSecret Plans"));

        net.configurePrivacyCloak(false, 0, "OnlyVault");
        net.applyPrivacyCloakConfig(cloak);
        TEST_ASSERT(cloak.isEnabled() == false);
        TEST_ASSERT(cloak.redactionStyle() == 0);
        TEST_ASSERT(cloak.matchesSensitiveTitle(L"OnlyVault Window"));

        std::error_code ec;
        std::filesystem::remove(id.configFilePath(), ec);
    }

    // #20 & #21: Cross-thread callbacks do not race or UAF, and drain on onTick()
    {
        IdentityManager id(4902);
        id.loadOrCreate();
        NetworkEngine net(id);
        FixGShell shell;

        {
            AppUi app(id, net, shell, "");
            std::thread worker([&]() {
                app.onPeerVersionMismatch("v9.9.9", "v1.0.0");
                app.onVersionUpgradeNotice("9.9.9");
            });
            worker.join();

            app.onTick();
            ui::RecordingCanvas rc;
            for (int i = 0; i < 30; ++i) app.render(rc, 1280, 800, 1.0 / 60.0);
            std::string frame = rc.serialize();
            TEST_ASSERT(frame.find("Dismiss") != std::string::npos);

            // A newer release is advice: it must not turn the dismissible peer-mismatch modal into a hard lock
            UpdateInfo reqInfo{};
            reqInfo.success = true;
            reqInfo.updateRequired = true;
            reqInfo.latestVersion = "v9.9.9";
            app.onUpdateCheckDone(reqInfo, false);

            ui::RecordingCanvas rc2;
            for (int i = 0; i < 10; ++i) app.render(rc2, 1280, 800, 1.0 / 60.0);
            std::string frame2 = rc2.serialize();
            TEST_ASSERT(frame2.find("Dismiss") != std::string::npos);
            TEST_ASSERT(frame2.find("Exit CppDesk") == std::string::npos);
        }

        std::error_code ec;
        std::filesystem::remove(id.configFilePath(), ec);
    }

    // #24 & #186: '?' shortcut does not hijack focused text inputs; isTextInputFocused reflects keyboard focus
    {
        IdentityManager id(4903);
        id.loadOrCreate();
        NetworkEngine net(id);
        FixGShell shell;
        AppUi app(id, net, shell, "");

        // Initial focusedField_ is FocusedField::RemoteId (a text input), even without mouse hover
        TEST_ASSERT(app.isTextInputFocused() == true);

        shell.shiftDown = true;
        app.onKey(KeyEvent{0xBF, 0, true, false});
        ui::RecordingCanvas rc;
        for (int i = 0; i < 20; ++i) app.render(rc, 1280, 800, 1.0 / 60.0);
        TEST_ASSERT(rc.serialize().find("Keyboard Shortcuts") == std::string::npos);

        std::error_code ec;
        std::filesystem::remove(id.configFilePath(), ec);
    }

    // #92: SystemService caches serviceState() and renders Pending as "Pending..."
    {
        IdentityManager id(4904);
        id.loadOrCreate();
        NetworkEngine net(id);
        FixGShell shell;
        shell.svcState = ui::ServiceState::Pending;
        AppUi app(id, net, shell, "");
        app.applySnapshotState("settings_system");

        ui::RecordingCanvas rc;
        for (int i = 0; i < 15; ++i) app.render(rc, 1280, 800, 1.0 / 60.0);
        TEST_ASSERT(shell.serviceStateCalls == 1);
        TEST_ASSERT(rc.serialize().find("Pending...") != std::string::npos);

        std::error_code ec;
        std::filesystem::remove(id.configFilePath(), ec);
    }

    // #91: Out-of-range pairedAtUnix falls back safely to "unknown"
    {
        IdentityManager id(4905);
        id.loadOrCreate();
        PairedPeer badPeer{};
        badPeer.deskId = 123456789ULL;
        badPeer.hostname = "CorruptTimePC";
        badPeer.pairedAtUnix = -999999999999LL;
        id.addPairedViewer(badPeer);

        NetworkEngine net(id);
        FixGShell shell;
        AppUi app(id, net, shell, "");
        app.applySnapshotState("settings_permissions");

        ui::RecordingCanvas rc;
        for (int i = 0; i < 30; ++i) app.render(rc, 1280, 800, 1.0 / 60.0);
        TEST_ASSERT(rc.serialize().find("paired unknown") != std::string::npos);

        std::error_code ec;
        std::filesystem::remove(id.configFilePath(), ec);
    }

    // #245: About section uses CPP_DESK_VERSION rather than hardcoded v3.12.0
    {
        IdentityManager id(4906);
        id.loadOrCreate();
        NetworkEngine net(id);
        FixGShell shell;
        AppUi app(id, net, shell, "");
        app.applySnapshotState("settings_about");

        ui::RecordingCanvas rc;
        for (int i = 0; i < 30; ++i) app.render(rc, 1280, 800, 1.0 / 60.0);
        std::string expectedVer = std::string("v") + CPP_DESK_VERSION;
        TEST_ASSERT(rc.serialize().find(expectedVer) != std::string::npos);

        std::error_code ec;
        std::filesystem::remove(id.configFilePath(), ec);
    }

    std::cout << "[FIX-G] All AppUi regression tests passed." << std::endl;
}
