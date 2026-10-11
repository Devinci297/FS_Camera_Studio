"""Narrow UI transplant shared by source and the known-loading simulator panel."""
CONTROLS = r'''
  function trackingPreference(view, value) {
    if(!view||readOnly())return;
    view.trackingPolicy=value;saveCamera(view);renderCameraTools();
  }
  function trackingOptions() {
    const view=current();if(!view)return;
    const body=openDialog('Head tracking · '+view.name);
    body.appendChild(make('p','Uses MSFS head tracking (TrackIR and compatible inputs). The simulator applies your head movement on top of this camera. Your original simulator setting is restored when camera control is released.','mc-note'));
    [['nativeIfQualified','Use simulator setting'],['on','On for this camera'],['off','Off for this camera']].forEach(([value,label])=>{
      const b=button(label,()=>{trackingPreference(view,value);closeDialog();render();},'mc-secondary');b.setAttribute('aria-pressed',String((view.trackingPolicy||'nativeIfQualified')===value));body.appendChild(b);
    });
    const status=S.caps.headTracking||{};if(status.error)body.appendChild(make('p',status.error,'mc-warn'));
    body.appendChild(make('p','This controls the MSFS tracking switch; it does not detect whether a physical tracker is connected. VR tracking and tools that move the camera independently are separate.','mc-note'));
    dialogActions([['Close',()=>closeDialog()]]);
  }
  function aircraftTrackingOptions(view) {
    if(!view)return;const body=openDialog('Aircraft tracking · '+view.name);
    body.appendChild(make('p','The camera stays at its airport position. Choose how it follows your aircraft. Right-drag temporarily overrides tracking.','mc-note'));
    [['off','Fixed direction'],['heading','Track heading only'],['both','Track heading and pitch']].forEach(([value,label])=>{
      const b=button(label,()=>{view.aircraftTracking=value;saveCamera(view);closeDialog();render();},'mc-secondary');b.setAttribute('aria-pressed',String((view.aircraftTracking||'off')===value));body.appendChild(b);
    });dialogActions([['Close',()=>closeDialog()]]);
  }
  async function saveAdjustedViews() {
    if(!S.snapshot||S.pendingCommit)return;
    await whenIdle();
    if(S.ownership.state==='Active') {
      const ack=await command('BakeTemporaryLook',{});
      if(ack.status!=='Completed'){announce(reasonText(ack.reason),'error');return;}
    }
    await saveChanges();
  }
  function renderCameraTools() {
    [['mc-editor-mouse-look',current],['mc-mouse-look',dockCamera]].forEach(([anchorId,getView])=>{
      const anchor=$(anchorId);if(!anchor)return;const view=getView(),prefix=anchorId+'-';
      const ensure=(key,label,action)=>{let b=$(prefix+key);if(!b){b=button(label,action,'mc-secondary');b.id=prefix+key;anchor.parentNode.insertBefore(b,anchor);}return b;};
      const status=S.caps.headTracking||{},live=view&&S.active.cameraId===view.id&&S.ownership.state==='Active';
      const policy=view&&view.trackingPolicy||'nativeIfQualified';
      const enabled=live&&typeof status.enabled==='boolean'?status.enabled:policy==='on';
      const head=ensure('head','Head tracking',()=>{const v=getView();if(v)trackingPreference(v,(v.trackingPolicy==='on'||v.trackingPolicy!=='off'&&S.caps.headTracking&&S.caps.headTracking.enabled)?'off':'on');});
      head.textContent='Head tracking: '+(policy==='nativeIfQualified'&&!live?'Sim':enabled?'On':'Off');head.setAttribute('aria-pressed',String(enabled));head.disabled=!view||readOnly();head.title=status.error||'Toggle MSFS head tracking for this camera. Save to keep this preference.';
      const tracking=ensure('aircraft','Track aircraft',()=>aircraftTrackingOptions(getView()));tracking.hidden=!view||view.mode!=='airport';tracking.textContent='Track: '+(view&&view.aircraftTracking==='both'?'Aircraft':view&&view.aircraftTracking==='heading'?'Heading':'Off');tracking.disabled=readOnly();
      const automatic=ensure('auto','Auto switch',()=>command('SetAirportAuto',{enabled:!S.airports.autoSwitch}));automatic.hidden=!view||view.mode!=='airport';automatic.setAttribute('aria-pressed',String(!!S.airports.autoSwitch));automatic.disabled=!live;automatic.title='Select the nearest airport camera as the aircraft moves. Manual camera selection, movement or right-drag turns this off.';
      if(anchorId==='mc-mouse-look'){const save=ensure('save','Save',saveAdjustedViews);save.title='Save adjusted view and all pending changes (Ctrl+S)';save.disabled=!S.snapshot||!!S.pendingCommit;}
    });
    const select=$('mc-tracking');if(select){select.hidden=true;let b=$('mc-head-tracking-options');if(!b){b=button('Head-tracking options',trackingOptions,'mc-secondary');b.id='mc-head-tracking-options';select.parentNode.insertBefore(b,select);const note=select.parentNode.querySelector('small');if(note)note.textContent='Choose simulator default, on or off for this camera.';}b.disabled=readOnly();}
    const id=$('mc-view-id');if(id){id.max='2147483647';const note=id.parentNode.querySelector('small');if(note)note.textContent='0 = unassigned. Each camera can have its own binding slot.';}
  }
'''
MOUSE = r'''  function startMouseLook() {
    const view=IS_MINI?dockCamera():current();if(!view)return;
    const body=openDialog('Mouse look · '+view.name);
    body.appendChild(make('p','Hold the right mouse button and drag over the simulator scene. Choose what happens when you release it.','mc-note'));
    [['retain','Keep the adjusted view'],['reset','Return to saved view on release']].forEach(([value,label])=>{
      const b=button(label,()=>{view.mouseLookOnRelease=value;saveCamera(view);closeDialog();render();},'mc-secondary');b.setAttribute('aria-pressed',String((view.mouseLookOnRelease||'retain')===value));body.appendChild(b);
    });
    body.appendChild(make('p','When revisiting this camera:','mc-note'));
    [['resetOnRecall','Start at saved view'],['retain','Remember this camera’s temporary look']].forEach(([value,label])=>{
      const b=button(label,()=>{view.temporaryLookPolicy=value;saveCamera(view);closeDialog();render();},'mc-secondary');b.setAttribute('aria-pressed',String((view.temporaryLookPolicy||'resetOnRecall')===value));body.appendChild(b);
    });
    body.appendChild(make('p','Use Save or Ctrl+S while at the adjusted view to store it permanently.','mc-note'));
    dialogActions([['Close',()=>closeDialog()]]);
  }
'''
def patch(text):
    def replace(old,new):
        nonlocal text
        assert text.count(old)==1,(old[:90],text.count(old))
        text=text.replace(old,new,1)
    replace('  function dockCamera()',CONTROLS+'\n  function dockCamera()')
    start=text.index('  function startMouseLook() {');end=text.index('  window.fscsStopMouseLook',start)
    text=text[:start]+MOUSE+text[end:]
    old_handler="    $('mc-editor-mouse-look').addEventListener('click', () => {"
    if old_handler in text:
        start=text.index(old_handler);end=text.index("    $('mc-mouse-look-area').addEventListener",start)
        text=text[:start]+"    $('mc-editor-mouse-look').addEventListener('click', startMouseLook);\n"+text[end:]
    replace('    renderViewButton();\n    renderTempLook();','    renderViewButton();\n    renderCameraTools();\n    renderTempLook();')
    replace("  function renderDock() {\n    const dock", "  function renderDock() {\n    renderCameraTools();\n    const dock")
    replace("    ['toggleMouseLook',", "    ['saveViews', 'Save adjusted view and all changes', 'Ctrl+S stores the current position, look and zoom, then saves pending changes'],\n    ['toggleHeadTracking', 'Toggle head tracking', 'Toggle MSFS head tracking for the active camera'],\n    ['toggleAirportAuto', 'Toggle auto airport camera switching', 'Follow the nearest camera while in an airport view'],\n    ['toggleMouseLook',")
    replace("    attachScrollbar($('mc-dock-favorites'), 'x');", "    attachScrollbar($('mc-dock-favorites'), IS_MINI ? 'y' : 'x');\n    if(IS_MINI){attachScrollbar($('mc-dock-categories'));attachScrollbar($('mc-mini-panel').querySelector('.mc-dock-tools'),'x');}")
    replace('let id = 1; while (id <= 20 && used.indexOf(id) >= 0) id++;','let id = 1; while (used.indexOf(id) >= 0) id++;')
    text=text.replace("        if (id > 20) { announce('All 20 view IDs are in use. Free one on the Behavior tab before assigning this camera.', 'warn'); return; }\n",'')
    if 'value>20' in text:
        replace('value>20','value>2147483647')
        replace('Use a whole View ID from 0 to 20.','Use 0 for unassigned, or a positive whole View ID.')
    else:
        replace("    for (let value = 0; value <= 20; value++) { const option = make('option', value ? String(value) : 'Unassigned'); option.value = String(value); $('mc-view-id').appendChild(option); }", "    { const old=$('mc-view-id'), input=make('input');input.id=old.id;input.type='number';input.min='0';input.max='2147483647';input.step='1';old.parentNode.replaceChild(input,old); }")
        replace('      const conflict = value > 0 && cameras().filter', '      if(!Number.isInteger(value)||value<0||value>2147483647){e.target.value=String(view.viewId||0);announce("Use 0 for unassigned, or a positive whole View ID.","warn");return;}\n      const conflict = value > 0 && cameras().filter')
    replace("&& !dialogOpen() && !IS_MINI) { event.preventDefault(); saveChanges(); }", r"&& !dialogOpen() && findBinding('saveViews') && findBinding('saveViews').input.kind==='key' && /^(VK_LCONTROL|VK_CONTROL)\+S$/.test(findBinding('saveViews').input.definition) && !event.shiftKey) { event.preventDefault(); if(!event.repeat && (bridge.demo || isTextEntry(event.target)))saveAdjustedViews(); }")
    # Immediate toggle state refresh on either panel.
    replace('AirportsChanged(p) { S.airports=p;', 'AirportsChanged(p) { S.airports=p; renderCameraTools();')
    return text

if __name__=='__main__':
    from pathlib import Path
    p=Path(__file__).resolve().parents[2]/'ui/app.js'
    p.write_text(patch(p.read_text(encoding='utf-8')),encoding='utf-8')
