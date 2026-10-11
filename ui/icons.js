/*
 * FS Camera Studio - inline SVG icon set.
 * 24x24 viewBox, stroke = currentColor, stroke-width 2, round caps/joins, no fill.
 * icon(name) > <svg> element; hydrateIcons(root) replaces <i data-icon="name"> placeholders.
 * Uses only DOM Level 2 calls (setAttribute/appendChild/replaceChild) so it runs in Coherent GT.
 */
(function (root) {
  'use strict';

  const P = d => ['path', { d }];
  const C = (cx, cy, r) => ['circle', { cx, cy, r }];
  const R = (x, y, width, height, rx) => ['rect', { x, y, width, height, rx }];
  const dot = (x, y) => P('M' + x + ' ' + y + 'h.01');

  const ICONS = {
    aperture: [C(12, 12, 9.5), P('M15.64 14.1 6.22 19.54M12 16.2 2.58 10.76M8.36 14.1V3.22M8.36 9.9l9.42-5.44M12 7.8l9.42 5.44M15.64 9.9v10.88')],
    plane: [P('M12 2.5c.9 0 1.5 1 1.5 2.2v4.8l7.5 4.5v2l-7.5-2.2V18l2.5 2v1.5L12 20.6l-4 .9V20l2.5-2v-4.2L3 16v-2l7.5-4.5V4.7c0-1.2.6-2.2 1.5-2.2z')],
    map: [P('M3 6.5 9 4l6 2.5L21 4v13.5L15 20l-6-2.5L3 20z'), P('M9 4v13.5M15 6.5V20')],
    'cloud-sun': [P('M12 2.5v1.5M5.6 5.1l1.1 1.1M3 11.5h1.5M18.4 5.1l-1.1 1.1'), P('M8 11a4 4 0 0 1 7.6-1.7'), P('M9.5 21h8a3.5 3.5 0 0 0 0-7h-.6a5 5 0 0 0-9.4 1.6A2.8 2.8 0 0 0 9.5 21z')],
    layers: [P('m12 3 9 4.5-9 4.5-9-4.5z'), P('m3 12 9 4.5 9-4.5'), P('m3 16.5 9 4.5 9-4.5')],
    'chevron-down': [P('m6 9 6 6 6-6')],
    'chevron-up': [P('m6 15 6-6 6 6')],
    'chevron-left': [P('m15 6-6 6 6 6')],
    'chevron-right': [P('m9 6 6 6-6 6')],
    'panel-bottom': [R(3, 3.5, 18, 17, 2.5), P('M3 15h18')],
    x: [P('M6 6l12 12M18 6 6 18')],
    camera: [P('M4 7.5h3l1.6-2.5h6.8L17 7.5h3a1.5 1.5 0 0 1 1.5 1.5v9a1.5 1.5 0 0 1-1.5 1.5H4A1.5 1.5 0 0 1 2.5 18V9A1.5 1.5 0 0 1 4 7.5z'), C(12, 13, 3.5)],
    keyboard: [R(2.5, 5.5, 19, 13, 2), dot(6.5, 9.5), dot(10, 9.5), dot(14, 9.5), dot(17.5, 9.5), dot(6.5, 12.5), dot(17.5, 12.5), P('M10 12.5h4M8 15.5h8')],
    'folder-open': [P('M3.5 19V6a1.5 1.5 0 0 1 1.5-1.5h4l2 2.5h6.5A1.5 1.5 0 0 1 19 8.5V10'), P('M3.5 19 6.2 11.4a1.5 1.5 0 0 1 1.4-1h13a1 1 0 0 1 .95 1.3L19.3 19z')],
    'settings-2': [P('M4 7h9M17 7h3M4 17h3M11 17h9'), C(15, 7, 2), C(9, 17, 2)],
    'list-tree': [P('M4 4.5h7M7 4.5v12.5h4M7 11h4M15 11h6M15 17h6M15 4.5h6')],
    plus: [P('M12 5v14M5 12h14')],
    minus: [P('M5 12h14')],
    search: [C(10.5, 10.5, 6.5), P('m15.5 15.5 4.5 4.5')],
    star: [P('M12 3l2.47 6.2 6.66.43-5.14 4.27 1.65 6.47L12 16.8l-5.64 3.57 1.65-6.47-5.14-4.27 6.66-.43z')],
    ellipsis: [C(5.5, 12, 1.2), C(12, 12, 1.2), C(18.5, 12, 1.2)],
    'rotate-ccw': [P('M4 12a8 8 0 1 0 2.4-5.7L4 8.5'), P('M4 3.5v5h5')],
    crosshair: [C(12, 12, 8.5), P('M12 3.5v4M12 16.5v4M3.5 12h4M16.5 12h4')],
    zap: [P('M13.5 2.5 4.5 13.5h7l-1 8 9-11h-7z')],
    'move-right': [P('M3 12h16.5'), P('m15.5 8 4 4-4 4')],
    spline: [P('M7 18c6 0 4-12 10-12'), C(5, 18, 2), C(19, 6, 2)],
    play: [P('M7.5 4.8v14.4a.8.8 0 0 0 1.2.7l11-7.2a.8.8 0 0 0 0-1.4l-11-7.2a.8.8 0 0 0-1.2.7z')],
    'circle-stop': [C(12, 12, 9.5), R(9, 9, 6, 6, 1)],
    check: [P('M5 12.5 9.5 17 19 7.5')],
    'check-circle-2': [C(12, 12, 9.5), P('m8 12.4 2.8 2.8 5.7-5.9')],
    pin: [P('M9 3h6M10 3v6l-3.5 4.2h11L14 9V3M12 13.2V21')],
    'panel-top-open': [R(3, 3.5, 18, 17, 2.5), P('M3 9h18'), P('m9 15.5 3-3 3 3')],
    'map-pin': [P('M12 21.5s-7-6.1-7-11.5a7 7 0 0 1 14 0c0 5.4-7 11.5-7 11.5z'), C(12, 10, 2.5)],
    download: [P('M12 3.5v11'), P('m7.5 10.5 4.5 4.5 4.5-4.5'), P('M4 16.5v2a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2')],
    upload: [P('M12 15V4'), P('m7.5 8.5 4.5-4.5 4.5 4.5'), P('M4 16.5v2a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-2')],
    history: [P('M3.5 12a8.5 8.5 0 1 0 2.5-6L3.5 8.5'), P('M3.5 4v4.5H8'), P('M12 8v4.5l3 2')],
    pencil: [P('M15.5 4.5l4 4L8 20H4v-4z'), P('m13.5 6.5 4 4')],
    copy: [R(8.5, 8.5, 12, 12, 2), P('M15.5 8.5v-3a2 2 0 0 0-2-2h-8a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h3')],
    'arrow-up': [P('M12 19.5v-15'), P('m5.5 11 6.5-6.5 6.5 6.5')],
    'arrow-down': [P('M12 4.5v15'), P('m5.5 13 6.5 6.5 6.5-6.5')],
    'folder-input': [P('M3 9V6a1.5 1.5 0 0 1 1.5-1.5h4l2 2.5h9A1.5 1.5 0 0 1 21 8.5v10a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 0 0 1 3 18.5V17'), P('M2 13h10'), P('m9 10 3 3-3 3')],
    'folder-plus': [P('M4.5 20h15a1.5 1.5 0 0 0 1.5-1.5v-10A1.5 1.5 0 0 0 19.5 7h-9l-2-2.5h-4A1.5 1.5 0 0 0 3 6v12.5A1.5 1.5 0 0 0 4.5 20z'), P('M12 10.5v6M9 13.5h6')],
    'trash-2': [P('M4 6.5h16M9 6.5v-2h6v2'), P('m6 6.5 1 13a1.5 1.5 0 0 0 1.5 1.5h7a1.5 1.5 0 0 0 1.5-1.5l1-13'), P('M10 11v6M14 11v6')],
    'alert-triangle': [P('M10.3 4.2 2.6 17.5a2 2 0 0 0 1.7 3h15.4a2 2 0 0 0 1.7-3L13.7 4.2a2 2 0 0 0-3.4 0z'), P('M12 9.5v4'), dot(12, 17)],
    plug: [P('M9 2.5v5M15 2.5v5'), P('M6 7.5h12v3a6 6 0 0 1-12 0z'), P('M12 16.5v5')],
    info: [C(12, 12, 9.5), P('M12 11v5.5'), dot(12, 7.8)],
    radio: [C(12, 12, 2), P('M7.8 16.2a6 6 0 0 1 0-8.4M16.2 7.8a6 6 0 0 1 0 8.4M5 19a10 10 0 0 1 0-14M19 5a10 10 0 0 1 0 14')],
  };

  const SVG_NS = 'http://www.w3.org/2000/svg';

  function icon(name, className) {
    const svg = document.createElementNS(SVG_NS, 'svg');
    svg.setAttribute('viewBox', '0 0 24 24');
    svg.setAttribute('width', '24');
    svg.setAttribute('height', '24');
    svg.setAttribute('fill', 'none');
    svg.setAttribute('stroke', 'currentColor');
    svg.setAttribute('stroke-width', '2');
    svg.setAttribute('stroke-linecap', 'round');
    svg.setAttribute('stroke-linejoin', 'round');
    svg.setAttribute('aria-hidden', 'true');
    svg.setAttribute('focusable', 'false');
    svg.setAttribute('class', 'mc-icon' + (className ? ' ' + className : ''));
    svg.setAttribute('data-icon', name);
    const shapes = ICONS[name];
    if (!shapes) {
      if (root.console) console.warn('Unknown icon: ' + name);
      return svg;
    }
    for (let i = 0; i < shapes.length; i++) {
      const el = document.createElementNS(SVG_NS, shapes[i][0]);
      const attrs = shapes[i][1];
      Object.keys(attrs).forEach(key => el.setAttribute(key, String(attrs[key])));
      svg.appendChild(el);
    }
    return svg;
  }

  /** Replace every <i data-icon="name"> inside scope with its SVG (keeps id and class). */
  function hydrateIcons(scope) {
    const list = (scope || document).querySelectorAll('i[data-icon]');
    for (let i = 0; i < list.length; i++) {
      const placeholder = list[i];
      const svg = icon(placeholder.getAttribute('data-icon'), placeholder.className);
      if (placeholder.id) svg.id = placeholder.id;
      placeholder.parentNode.replaceChild(svg, placeholder);
    }
  }

  root.FSCIcons = { icon, hydrateIcons, names: Object.keys(ICONS) };
})(typeof globalThis !== 'undefined' ? globalThis : window);
