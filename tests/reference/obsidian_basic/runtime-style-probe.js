// Paste into the local reference vault's developer console. Read-only DOM/CSS.
// Docking DevTools changes viewport width: do not use these bounds as idle-layout measurements.
console.log('REF_STYLE_JSON:' + JSON.stringify((() => {
  const line = document.querySelector('.markdown-source-view.mod-cm6 .cm-line');
  if (!line) throw new Error('No CM6 source editor line; confirm mode first');
  const style = getComputedStyle(line);
  const body = getComputedStyle(document.body);
  const names = ['font-text-size', 'font-text', 'font-monospace', 'line-height-normal',
    'line-height-tight', 'file-line-width', 'file-margins', 'p-spacing', 'heading-spacing',
    'text-selection', 'table-line-height', 'table-cell-padding', 'table-border-width',
    'h1-size', 'h2-size', 'h3-size', 'h4-size', 'h5-size', 'h6-size'];
  return { method: 'read-only-getComputedStyle-in-docked-DevTools',
    devicePixelRatio, fontSize: style.fontSize, lineHeight: style.lineHeight,
    fontFamily: style.fontFamily, variables: Object.fromEntries(names.map(name =>
      [name, body.getPropertyValue('--' + name).trim()])),
    note: 'No document contents or app internals read. Font fallback and actual visible presentation require separate observation.' };
})()));
