/*
 * FS Camera Studio — MSFS 2024 toolbar-panel workarounds.
 * Kept in one small function so it can be tuned during in-sim testing. No-op in a normal browser.
 */
(function (root) {
  'use strict';

  const HIDING_CLASSES = ['hide', 'panelInvisible'];

  /** Some MSFS 2024 toolbar windows stay hidden behind these classes; remove them shortly after load. */
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
      } catch (error) {
        if (root.console) console.warn('msfsPanelFixups failed', error);
      }
    }, 1000);
  }

  root.msfsPanelFixups = msfsPanelFixups;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', msfsPanelFixups);
  else msfsPanelFixups();
})(typeof globalThis !== 'undefined' ? globalThis : window);
