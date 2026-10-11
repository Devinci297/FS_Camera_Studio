# Save confirmation audio

The simulator toolbar uses a native confirmation chime. Browser previews can play the
packaged `ui/audio/cameras-saved.wav` voice. A successful browser WAV decode is not
evidence that Coherent can play that file in an in-game toolbar view.

The native path is based on the locally installed MSFS 2024 shared UI, inspected on
2026-10-11 at
`C:/Program Files (x86)/Steam/steamapps/common/MSFS2024/Packages/fs-base-ui/html_ui/Global/lib.js`.
This shipped bundle's `SoundListener` registers `JS_LISTENER_SOUND`; its
`playSound(name)` calls `this.call("PLAY_SOUND", name)`. The current `SoundManager`
sends `UI_VALID` through that listener for a normal confirmation.

The addon registers that same listener and waits for its connected callback before
calling `listener.call("PLAY_SOUND", "UI_VALID")`. It also handles registration
failure, rejected calls and four-second readiness/call timeouts. The legacy
`JS/common.js` ViewListener implementation (lines 4146 onward) does not wait for
readiness inside `call`, which is why the addon explicitly waits. The old
`PLAY_SOUND_FROM_VIEW` fire-and-forget route found in legacy `common.js` was silent
in the user's toolbar; it is no longer used. No custom Wwise event is invented.

The official [JavaScript overview](https://docs.flightsimulator.com/msfs2024/flighting/programming-apis/javascript/javascript/)
identifies shared simulator JavaScript as the reference for these APIs. The documented
[sound configuration](https://docs.flightsimulator.com/html/Content_Configuration/Sounds/Sounds.htm)
and [sound examples](https://docs.flightsimulator.com/html/Content_Configuration/Sounds/Sound_Examples.htm)
describe custom sounds attached to a SimObject's sound configuration; they do not
establish arbitrary WAV playback support for a standalone toolbar addon.

`FSCSSaveFeedback.getStatus()` distinguishes an accepted native sound request from
browser playback or unavailable audio. Resolving the sound API call is not proof
of audible output, so it reports **requested**, never **heard**. Settings includes a
Test sound button. Simulator UI sound volume can affect the result. Hidden-panel
audibility still requires a live simulator check; there is no visibility gate in
the inspected JavaScript sound path.

Tests cover native registration readiness (including a synchronous callback),
dispatch, duplicate completion events, rejected calls, missing services, timeouts,
late registration without stale playback, and browser WAV decoding. Audibility
must still be checked in the simulator. This chime route does not speak the
packaged words: arbitrary voice playback is still a browser-only feature.
