# Wayland host: what to check on a real Wayland desktop

CI tests the sharing code against a stand-in for the desktop's screen-sharing service, but hosted machines have no real
Wayland desktop, so these need a person on GNOME and, if possible, KDE (Fedora and Ubuntu both log in to Wayland by
default). Use a second machine (Windows, Linux or Mac) as the viewer. Note the distribution, the desktop and its version,
and whether the screen is scaled (125%, 200%).

Build with `libpipewire-0.3-dev` installed (without it the video part is left out and the host falls back to X11), and
make sure the session really is Wayland: `echo $XDG_SESSION_TYPE` prints `wayland`.

## The question
- [ ] Connect from the viewer. The desktop asks whether to share the screen (and allow remote control). Until you answer, the viewer sees nothing and the dashboard says it is waiting for your permission.
- [ ] Answer no. Nothing is shared, the viewer is disconnected at once and shows "The host did not allow screen sharing.", the host shows a message, Settings -> System & Service says screen sharing is off, and the viewer does not reconnect on its own.
- [ ] Connect again and answer yes: the viewer sees the screen. Choose a monitor in the dialog if there are several.
- [ ] Untick remote control in the dialog (where the desktop offers that): the viewer sees the screen, the dashboard says control is off, and nothing the viewer types or clicks reaches the desktop.
- [ ] The viewer disconnects while the question is still open: the question goes away by itself.
- [ ] On a desktop with a newer portal (GNOME 43+ or KDE Plasma 5.27+ with xdg-desktop-portal 1.15+): the second session does not ask again.

## Seeing the screen
- [ ] The picture is sharp, at the right size and aspect, also on a scaled screen.
- [ ] Moving a window, scrolling and playing a video look smooth; note the frame rate on the viewer.
- [ ] The mouse pointer appears once on the viewer (not twice).
- [ ] Changing the resolution or scale during a session keeps the picture right.
- [ ] Clicking "Stop sharing" in the desktop's own indicator ends the session: the viewer is disconnected with "The host stopped sharing the screen." and the host is told why.

## Controlling the desktop
- [ ] Mouse moves and clicks land exactly where the viewer points (compare near the corners, on a scaled screen, and on the second monitor if there is one).
- [ ] Dragging a window or selecting text works; right click opens the context menu; the scroll wheel scrolls in both directions (up on the viewer scrolls up).
- [ ] Typing letters, digits, symbols, Return, Backspace, Tab and Escape works; Ctrl+C / Ctrl+V / Ctrl+A act in the focused application.
- [ ] Ending the session while holding a key or button leaves nothing stuck.

## Fallbacks
- [ ] With the portal service stopped (or on a desktop without one), the host falls back to the X11 picture and the dashboard explains that only X applications are shown.
- [ ] In an Xorg session nothing changes: the same picture and control as before, no question asked.

## Report
Send the distribution, desktop and version, scaling, what failed and the terminal output of `cppdesk` if it printed any.
