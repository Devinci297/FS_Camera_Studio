# Included A320 starter views

Camera Studio includes a 12-view starter for the default/iniBuilds A320neo, FlyByWire A32NX, and Fenix A320. It recognizes A320, A32NX, and FNX 320 aircraft identifiers and saves a separate profile against each loaded aircraft package. It does not match A319 or A321 models.

The views use the simulator's pilot eyepoint for the loaded aircraft. They include normal, landing, wide cockpit, left window, right cockpit, captain displays, center displays, main panel, glareshield, overhead, pedestal, and left-side controls. All are interactive cockpit views; the first view in each category is its category default.

These are starter framings, not calibrated instrument closeups. Aircraft developers use different cockpit geometry and eyepoints. Adjust a view with the movement controls or mouse look, then save it for that aircraft. No copilot-seat or exterior coordinates are guessed from another model.

On first loading an A320 without an existing matching profile, Camera Studio creates and opens the starter without taking camera control. Existing saved profiles and their camera positions are preserved. If you already have a profile, use **Profiles → New profile** with the current aircraft selected to create an additional starter; **Save as new** continues to copy your current views.

Generation and persistence are covered by native tests. Visual framing in each installed simulator aircraft still needs tester confirmation.
