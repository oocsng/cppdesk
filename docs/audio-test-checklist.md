# Sound: what to check on real hardware

CI proves the sound code against a test sound server (Linux) and by compiling and running its logic (macOS), but it has no
real microphone, speakers or Bluetooth. These need a person. Use two machines (any mix of Windows, Linux and Mac) in
a room where you can hear both, and note the systems, the sound server on Linux (PulseAudio or PipeWire) and whether
headphones were used. Headphones avoid echo; skip them for the echo item below.

## Voice intercom (microphone and speaker)
- [ ] Start the voice intercom on the viewer: the host hears the viewer, and the viewer hears the host's room, clearly.
- [ ] The level meter on the viewer moves when you speak; muting the microphone silences it for the other side.
- [ ] Linux host: no sign of the microphone before the viewer talks; once the viewer talks, the dashboard's session box says
      "Microphone is on: the viewer can hear this room", and the line disappears when the viewer stops or the session ends,
      including when the network cable is pulled.
- [ ] Stopping the intercom returns at once (no frozen window), on every platform.
- [ ] Linux host: the viewer hears their own voice back about a quarter second late (expected: the host sends what its speakers play).

## The host's system sound
- [ ] Play music or a video on the host: the viewer hears it, in time with the picture, without crackling.
- [ ] Linux host on PulseAudio, and again on PipeWire (Ubuntu 22.04 and a newer desktop): the same.
- [ ] Mac host on macOS 13 or later: the same, and CppDesk's own sounds and the viewer's voice are not sent back.
- [ ] Mac host on macOS 12: the picture works as before and there is no host sound.
- [ ] Bluetooth headphones as the host's output: the viewer still hears the host's sound.

## The viewer
- [ ] Volume slider: lower, then higher, takes effect within a moment, on Windows, Linux and Mac viewers.
- [ ] Mute: silences at once; a viewer that is muted before it connects hears nothing until it unmutes.
- [ ] After the connection drops and comes back by itself, the viewer's mute and volume are still what they were.
- [ ] Pause the viewer's network for several seconds, then resume: the sound is not seconds behind (the newest quarter
      second is kept).
- [ ] Closing the session while sound is playing leaves nothing playing and nothing frozen.

## Without sound
- [ ] A Linux machine with no PulseAudio or PipeWire: the app starts, a session shows the screen, and sound is simply absent.
- [ ] A host with no sound device (a headless server): the same.
- [ ] Echo: host and viewer in the same room without headphones squeal or echo; this is expected (no echo cancellation).

## Report
Send the systems and versions, what failed, and for a crash the backtrace or crash report.
