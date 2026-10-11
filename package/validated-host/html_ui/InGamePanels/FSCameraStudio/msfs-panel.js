/*
 * FS Camera Studio - MSFS 2024 toolbar-panel workarounds.
 * Kept in one small function so it can be tuned during in-sim testing. No-op in a normal browser.
 */
(function (root) {
  'use strict';

  // Only the broken initial 'hide' is removed. panelInvisible belongs to the simulator's visibility lifecycle,
  // not to frame styling: the mini is borderless through no-header/no-background and scoped CSS instead.
  const HIDING_CLASSES = ['hide'];

  root.fscsSetPanelOpen = (surface, open) => {
    const panel = document.querySelector('ingame-ui');
    if (!panel || !root.Coherent || typeof root.Coherent.call !== 'function') return false;
    const id = surface === 'mini' ? 'PANEL_FSCS_CAMERA_MINI_DOCK' : 'PANEL_FSCS_CAMERA_STUDIO';
    if (!open && panel.panelID === id && typeof panel.closePanel === 'function') panel.closePanel();
    else root.Coherent.call('TOOLBAR_BUTTON_TOGGLE', id, open);
    return true;
  };

  // Use the stock game-owned drag handler on the application header only.
  // Controls (including dynamically-created camera rows and scrollbar thumbs) remain interactive.
  function editorDrag(panel) {
    const editor = document.getElementById('mc-drag-header');
    const handler = panel.dragDropHandler;
    if (!editor || !handler || editor.fscsDragHandler === handler) return;
    if (editor.fscsDragCleanup) editor.fscsDragCleanup();
    const interactive = 'button,input,textarea,select,a,summary,label,[contenteditable], [role="button"], [role="slider"], [draggable="true"], .mc-sb, .mc-dialog';
    const mark = event => {
      const control = event.target.closest && event.target.closest(interactive);
      if (control) control.classList.add('nodrag');
    };
    const down = event => { mark(event); if (event.button === 0) handler.startDrag(event); };
    const over = event => { mark(event); handler.checkCanDrag(event); };
    const blur = () => { handler.leaveDrag(); if (panel.isDragging && handler.dragEnd) handler.dragEnd(); };
    window.addEventListener('blur', blur);
    editor.addEventListener('mousedown', down);
    editor.addEventListener('mouseover', over);
    editor.addEventListener('mouseleave', handler.leaveDrag);
    editor.fscsDragCleanup = () => {
      window.removeEventListener('blur', blur);
      editor.removeEventListener('mousedown', down);
      editor.removeEventListener('mouseover', over);
      editor.removeEventListener('mouseleave', handler.leaveDrag);
    };
    editor.fscsDragHandler = handler;
  }

  function miniHitArea(panel) {
    if (panel.fscsHitArea) return;
    const editor = document.getElementById('mc-sim');
    const strip = document.getElementById('mc-mini-region');
    const dock = document.getElementById('mc-mini-panel');
    const dialog = document.getElementById('mc-dialog-shade');
    if (!editor || !strip || !dock || !dialog) return;
    // Keep the drawing viewport tall enough for the dock, but reserve only the visible controls and hover strip
    // for hit testing. Do not let the stock full-viewport report consume cockpit clicks above the dock.
    let lastReport = '', lastDiagnostic = '';
    panel.sendRect = () => {
      if (!root.Coherent || typeof root.Coherent.trigger !== 'function') return;
      // Stock ingameUi tracks panel hover separately from UPDATE_PANEL_RECT. Clear a stale claim on hide/off.
      const pointerDisabled = !panel.active || !panel.visible || editor.dataset.miniMode === 'off';
      if (pointerDisabled && !panel.fscsPointerReleased && panel.m_toolbar_listener) {
        panel.m_toolbar_listener.pushPanelMouseOut(panel.panelID);
        panel.fscsPointerReleased = true;
      } else if (!pointerDisabled) panel.fscsPointerReleased = false;
      let left = 0, right = 0, top = 0, bottom = 0;
      if (panel.active && panel.visible && editor.dataset.miniMode !== 'off') {
        const rect = strip.getBoundingClientRect();
        left = rect.left; right = rect.right; top = rect.top; bottom = rect.bottom;
        if (editor.dataset.miniOpen === 'true' || editor.dataset.miniMode === 'always') {
          top = Math.min(top, dock.getBoundingClientRect().top);
        }
        if (!dialog.hidden) {
          const bounds = (dialog.classList.contains("mc-tracking-shade") ? dialog.querySelector(".mc-dialog") : dialog).getBoundingClientRect();
          left = Math.min(left,bounds.left); right = Math.max(right,bounds.right); top = Math.min(top,bounds.top); bottom = Math.max(bottom,bounds.bottom);
        }
        if (editor.dataset.mouseLook === 'true') {
          left = 0; right = window.innerWidth; top = Math.max(96, window.innerHeight * .12); bottom = window.innerHeight;
        }
      }
      // The host can briefly report a tiny viewport during loading. Never publish inverted/outside hit bounds.
      const w = Math.max(0, window.innerWidth), h = Math.max(0, window.innerHeight);
      left = Math.max(0, Math.min(w, left)); right = Math.max(left, Math.min(w, right));
      top = Math.max(0, Math.min(h, top)); bottom = Math.max(top, Math.min(h, bottom));
      const report = JSON.stringify([panel.panelID, left, right, top, bottom, panel.active, panel.visible]);
      if (report !== lastReport) {
        lastReport = report;
        root.Coherent.trigger('UPDATE_PANEL_RECT', panel.panelID, left, right, top, bottom);
      }
      if (root.FSCApp && root.FSCApp.state.snapshot && report !== lastDiagnostic) {
        lastDiagnostic = report;
        root.FSCApp.bridge.send('PanelDiagnostics', { surface: 'mini', active: !!panel.active, visible: !!panel.visible,
          width: window.innerWidth, height: window.innerHeight, left, right, top, bottom, open: editor.dataset.miniOpen === 'true' });
      }
    };
    // Both-dimension SDK registration keeps the fixed drawing viewport; do not content-fit it to the hit strip.
    panel.sendUpdatePanelSize = () => {};
    panel.fscsHitArea = true;
    root.fscsUpdateMiniRect = () => {
      if ((!panel.active || !panel.visible) && editor.dataset.mouseLook === 'true' && root.fscsStopMouseLook) root.fscsStopMouseLook();
      panel.sendRect(); requestAnimationFrame(() => panel.sendRect());
    };
    new MutationObserver(root.fscsUpdateMiniRect).observe(editor, { attributes: true, attributeFilter: ['data-mini-open', 'data-mini-mode', 'data-mouse-look'] });
    new MutationObserver(root.fscsUpdateMiniRect).observe(dialog, { attributes: true, attributeFilter: ['hidden'] });
    // The stock visibility callback changes panelInvisible without sending a new rectangle or panelActive.
    // Refresh on visibility/size changes too, including delayed startup and reopening after a hidden frame.
    new MutationObserver(root.fscsUpdateMiniRect).observe(panel, { attributes: true, attributeFilter: ['class', 'style'] });
    window.addEventListener('resize', root.fscsUpdateMiniRect);
    if (typeof ResizeObserver === 'function') new ResizeObserver(root.fscsUpdateMiniRect).observe(dock);
    panel.addEventListener('panelActive', root.fscsUpdateMiniRect);
    panel.addEventListener('panelInactive', root.fscsUpdateMiniRect);
    root.fscsUpdateMiniRect();
  }

  // common.js enables a native blurred scene behind each Coherent view. CSS cannot
  // remove that compositor layer; disable it on this panel view before applying rgba.
  let clearBackgroundReady = false;
  function clearNativeBackground(force) {
    if (clearBackgroundReady && !force) return;
    if (typeof root.SetBlurredBackground !== 'function') return;
    root.SetBlurredBackground(false);
    clearBackgroundReady = true;
  }

  function hostReady() {
    const panel = document.querySelector('ingame-ui');
    if (!panel) return;
    clearNativeBackground(false);
    if (!panel.fscsClearBackgroundBound) {
      panel.addEventListener('panelActive', () => clearNativeBackground(true));
      panel.fscsClearBackgroundBound = true;
    }
    // The dataStorageReady event can precede asynchronous template attachment. Re-deliver it once the stock
    // host is connected and storage is ready, so its drag/resize setup cannot be missed.
    if (panel.headerElement && !panel.dragDropHandler && typeof root.GetDataStorage === 'function' && root.GetDataStorage()) {
      document.dispatchEvent(new Event('dataStorageReady'));
    }
    if (panel.id === 'FSCSMain') {
      editorDrag(panel);
      if (root.FSCApp && root.FSCApp.state.snapshot) {
        const visible = Boolean(panel.active && panel.visible);
        if (visible && !panel.fscsWasVisible) root.FSCApp.bridge.send('RelaySurface', {target:'mini',op:'stopMouseLook'});
        panel.fscsWasVisible = visible;
      }
    }
    if (panel.id === 'FSCSMini') { miniHitArea(panel); if (root.fscsUpdateMiniRect) root.fscsUpdateMiniRect(); }
  }

  /** Some MSFS 2024 toolbar windows start with a 'hide' class; remove it shortly after load. */
  function msfsPanelFixups() {
    const inSim = typeof root.RegisterCommBusListener === 'function' || Boolean(document.querySelector('ingame-ui'));
    if (!inSim) return;
    setTimeout(() => {
      try {
        const nodes = [];
        const start = document.getElementById('mc-sim') || document.body;
        for (let node = start; node && node.nodeType === 1; node = node.parentNode) nodes.push(node);
        const panels = document.querySelectorAll('ingame-ui');
        for (let i = 0; i < panels.length; i++) nodes.push(panels[i]);
        nodes.forEach(node => {
          HIDING_CLASSES.forEach(name => { if (node.classList && node.classList.contains(name)) node.classList.remove(name); });
        });
        hostReady();
      } catch (error) {
        if (root.console) console.warn('msfsPanelFixups failed', error);
      }
    }, 1000);
    document.addEventListener('dataStorageReady', () => requestAnimationFrame(hostReady));
    // Template and toolbar callbacks arrive independently, including when a panel is reopened.
    const timer = setInterval(hostReady, 250);
    window.addEventListener('unload', () => clearInterval(timer));
  }

  root.msfsPanelFixups = msfsPanelFixups;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', msfsPanelFixups);
  else msfsPanelFixups();
})(typeof globalThis !== 'undefined' ? globalThis : window);
