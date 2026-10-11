# Camera Studio 0.3.0 interface

The editor separates global head tracking, aircraft tracking and automatic airport switching from the current camera's tools. Airport cameras show when a tracking override applies; cockpit views indicate that aircraft tracking is excluded.

Settings has five sections: Appearance, Movement, Tracking, Airport defaults, and Saving & updates. Airport overrides show their effective value and Global/Override source, grouped by tracking, zoom and height.

Position and rotation buttons repeat while held and stop on release or focus loss. Keyboard hints follow configured bindings. Capture current view opens a review and applies to the draft; Save all changes commits the draft. The save shortcut hint follows its assigned keyboard binding.

Camera context actions prioritize key binding, visibility in the mini panel and copy/paste. Appearance contains the color and icon choices; deletion is separated. Profiles distinguish aircraft associations from the shared airport library.

The mini panel remains a shallow dock across the bottom of the screen. Categories stay beside cameras; only the camera list scrolls when crowded. Narrow layouts use a More menu for secondary tools while Auto switch and Save remain visible.

The main library has a clear hierarchy: aircraft/airport section headings, category headers with aligned counts and actions, and indented camera rows. Compact rows keep names and shortcut badges on one line; the active view has a small status indicator. Expansion controls reflect the actual category state. Main-panel icons and labels remain visible regardless of mini-panel category display preferences.

Binding dialogs have a visible footer, separate Primary and Secondary slots, and an in-panel controller list. Long content scrolls above the Assign/Replace/Cancel actions. The input badge updates to the assigned shortcut.

Browser checks verify layout and draft persistence. Native compositor transparency, pointer behavior and physical controller capture still require testing in MSFS. Version 0.3.0 includes a rebuilt native module and the reviewed packaged host.

The packaged Behavior-tab mouse-look release choices are now connected to draft updates and persist on Save all changes.
