# Mac host: what to check on a real Mac

CI can build the Mac app and test its logic, but hosted machines cannot grant Screen Recording or Accessibility, so
these need a person on a Mac. Use the `CppDesk-mac-untested` artifact (see the README) and a second machine (Windows,
Linux or another Mac) as the viewer. Note the macOS version and whether the Mac is a laptop or a desktop.

## Permissions
- [ ] First launch: the dashboard shows "Allow screen sharing" with two rows.
- [ ] "Allow..." for Screen Recording opens the right System Settings page; after turning it on and coming back, the card offers "Restart CppDesk"; after the restart the Screen Recording row is gone.
- [ ] "Allow..." for Accessibility shows macOS's own prompt, and the row disappears within a second of turning it on (no restart needed).
- [ ] Settings -> System & Service lists both permissions with the right state.
- [ ] A connection request while a permission is off shows the warning line before Accept.

## Seeing the screen
- [ ] The viewer sees the Mac desktop, sharp, at the right size and aspect (also on a Retina laptop screen).
- [ ] Moving a window, scrolling and playing a video look smooth; note the frame rate on the viewer.
- [ ] Two displays: switching the monitor on the viewer shows the other display, with clicks landing correctly.
- [ ] Putting the Mac to sleep and waking it, or locking and unlocking, brings the picture back (or ends the session cleanly).
- [ ] Turning Screen Recording off in System Settings during a session ends the picture and the card comes back.

## Controlling the Mac
- [ ] Mouse moves and clicks land exactly where the viewer points (compare on a Retina screen and on a second display).
- [ ] Dragging a window or selecting text works (real drag, not a series of clicks); double click opens a file, triple click selects a line.
- [ ] Right click opens the context menu; the scroll wheel scrolls in both directions.
- [ ] Typing letters, digits, symbols and Return/Delete/Tab/Escape works.
- [ ] From a Windows viewer: Ctrl+C / Ctrl+V / Ctrl+A act as Command+C / V / A on the Mac; the Windows key acts as Control.
- [ ] Ending the session while holding a key or button leaves nothing stuck on the Mac.

## Report
Send the macOS version, hardware, what failed and, if the app crashed, the crash report from Console.app.
