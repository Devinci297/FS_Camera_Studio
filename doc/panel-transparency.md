# Simulator panel transparency

The opacity sliders control one rgba background on each Camera Studio panel. Text and controls keep their own contrast. Host wrappers, pseudo-elements and gradients are transparent.

MSFS 2024 also applies a native scene blur outside CSS. Its shared `html_ui/JS/common.js` defines `SetBlurredBackground` as `Coherent.trigger("SET_BLURRED_BACKGROUND", value)` and calls it with `true` at load. Clearing CSS `backdrop-filter` cannot disable that layer.

Camera Studio calls the simulator's `SetBlurredBackground(false)` after its ingame-ui host becomes available, and again when the panel is reopened. Each main/mini page sets up its own view; no simulator files are edited. A late-loading API is retried, and browser previews do nothing.

`ui/tests/native-transparency.test.cjs` checks both panel routes, reopening, delayed API availability and the browser no-op. `main-opacity.test.cjs` checks independent alpha values and opaque text. These tests verify routing and CSS; the final composited image still needs visual verification in MSFS.
