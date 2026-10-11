"""Small shared patch for source UI and the older, known-loading packaged UI."""
def patch_app(text):
    def replace(old, new):
        nonlocal text
        assert text.count(old) == 1, (old[:100], text.count(old))
        text = text.replace(old, new, 1)
    if "['moveFast', 'Faster movement (hold)']" not in text:
        replace("['zoomOut', 'Zoom out']];", "['zoomOut', 'Zoom out'], ['moveFast', 'Faster movement (hold)']];")
    replace("function setPrecision(value) {", "function setPrecision(value, persist) {")
    replace("    S.precision = value;", """    S.precision = value;
    if (persist && S.working) {
      S.working.settings.ui.precision = value;
      saveSettings();
    }""")
    replace("setPrecision(control.getAttribute('data-precision'))", "setPrecision(control.getAttribute('data-precision'), true)")
    replace("    const ui = S.working.settings.ui;\n    root.dataset.shortcuts", "    const ui = S.working.settings.ui;\n    if (S.precision !== ui.precision) setPrecision(ui.precision);\n    root.dataset.shortcuts")
    replace("function keyCapture(event, held) {", "function keyCapture(event, held, allowShift) {")
    replace("    if (MODIFIERS[event.code]) { held[event.code] = true; return { pending: true }; }", """    if (allowShift && (event.code === 'ShiftLeft' || event.code === 'ShiftRight') && !event.ctrlKey && !event.altKey) {
      return { input: { kind: 'key', definition: MODIFIERS[event.code][0], label: MODIFIERS[event.code][1] } };
    }
    if (MODIFIERS[event.code]) { held[event.code] = true; return { pending: true }; }""")
    replace("keyCapture(event, S.assign.held)", "keyCapture(event, S.assign.held, spec.action === 'moveFast')")
    replace("    body.appendChild(note);\n    body.appendChild(make('div', 'Or use a controller'", """    body.appendChild(note);
    if (spec.action === 'moveFast') body.appendChild(make('p', 'Press Shift on its own, or choose another key to hold for 4x faster movement.', 'mc-note'));
    if (spec.action === 'zoomIn' || spec.action === 'zoomOut') {
      const wheelNote = 'Uses MSFS zoom commands. Keep the wheel assigned to zoom in MSFS Controls. Scroll over this capture field to choose a direction, or use the buttons below.';
      body.appendChild(make('p', wheelNote, 'mc-note'));
      const chooseWheel = up => stageInput({kind:'simZoom', definition:up?'sim:ZOOM_IN':'sim:ZOOM_OUT', label:up?'Mouse wheel up / MSFS zoom in':'Mouse wheel down / MSFS zoom out'}, null);
      const wheelActions = make('div', undefined, 'mc-dialog-actions');
      add(wheelActions, button('Wheel up', () => chooseWheel(true), 'mc-secondary'), button('Wheel down', () => chooseWheel(false), 'mc-secondary'));
      body.appendChild(wheelActions);
      capture.addEventListener('wheel', event => { if (!event.deltaY) return; event.preventDefault(); event.stopPropagation(); chooseWheel(event.deltaY < 0); }, {passive:false});
    }
    body.appendChild(make('div', 'Or use a controller'""")
    replace("    const query = $('mc-binding-search').value.trim().toLowerCase();", """    if (!$('mc-binding-kind').querySelector('[value="simZoom"]')) {
      const option = make('option', 'Mouse wheel / MSFS zoom'); option.value = 'simZoom'; $('mc-binding-kind').appendChild(option);
    }
    const query = $('mc-binding-search').value.trim().toLowerCase();""")
    replace("detail: entry[0].indexOf('zoom') === 0 ? 'Field of view'", "detail: entry[0] === 'moveFast' ? 'Hold for 4x movement speed' : entry[0].indexOf('zoom') === 0 ? 'Field of view'")
    replace("? 'Temporary look' : 'Temporary position offset'", "? 'Temporary look' : 'Fine 0.1x / Normal 1x / Coarse 10x'")
    return text

def patch_bridge(text):
    old = '    return { schemaVersion: 2, bindings: list };'
    assert text.count(old) == 1
    return text.replace(old, """    [['moveForward','W'],['moveBack','S'],['moveLeft','A'],['moveRight','D'],['moveDown','Q'],['moveUp','E'],['moveFast','VK_SHIFT']].forEach(entry => list.push({action:entry[0],input:{kind:'key',definition:entry[1],label:entry[1]==='VK_SHIFT'?'Shift':entry[1]}}));
    [['zoomIn','sim:ZOOM_IN','Mouse wheel up / MSFS zoom in'],['zoomOut','sim:ZOOM_OUT','Mouse wheel down / MSFS zoom out']].forEach(entry => list.push({action:entry[0],input:{kind:'simZoom',definition:entry[1],label:entry[2]}}));
    return { schemaVersion: 2, movementDefaultsVersion: 1, bindings: list };""", 1)

if __name__ == '__main__':
    from pathlib import Path
    root = Path(__file__).resolve().parents[2]
    for name, transform in [('app.js', patch_app), ('bridge.js', patch_bridge)]:
        path = root / 'ui' / name
        path.write_text(transform(path.read_text(encoding='utf-8')), encoding='utf-8')
