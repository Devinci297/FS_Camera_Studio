"""Create the illustrated tester manual from captured release UI images."""
from pathlib import Path
from reportlab.pdfgen import canvas
from reportlab.lib import colors
from reportlab.lib.styles import ParagraphStyle
from reportlab.platypus import Paragraph
from reportlab.lib.utils import ImageReader
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'output/pdf/FS-Camera-Studio-Manual.pdf'
OUT.parent.mkdir(parents=True,exist_ok=True)
IMG=ROOT/'doc/manual/images'
W,H=595.28,841.89
c=canvas.Canvas(str(OUT),pagesize=(W,H))
c.setTitle('FS Camera Studio - Illustrated User Manual')
c.setAuthor('FS Camera Studio')
NAVY=colors.HexColor('#102331');TEAL=colors.HexColor('#087a70');GRAY=colors.HexColor('#526576')
style=ParagraphStyle('body',fontName='Helvetica',fontSize=10.5,leading=15,textColor=NAVY,spaceAfter=8)
small=ParagraphStyle('caption',parent=style,fontSize=8.5,leading=11,textColor=GRAY)
page=0
def start(title,subtitle):
 global page,y
 page+=1;c.setFillColor(NAVY);c.rect(0,H-104,W,104,fill=1,stroke=0)
 c.setFillColor(colors.HexColor('#67dbc5'));c.setFont('Helvetica-Bold',10);c.drawString(38,H-29,'FS CAMERA STUDIO  /  MSFS 2024')
 c.setFillColor(colors.white);c.setFont('Helvetica-Bold',24);c.drawString(38,H-62,title)
 c.setFont('Helvetica',10);c.drawString(38,H-83,subtitle)
 c.setFillColor(GRAY);c.setFont('Helvetica',8);c.drawString(38,23,'Version 0.3.0  |  11 October 2026');c.drawRightString(W-38,23,str(page))
 c.bookmarkPage('page'+str(page));c.addOutlineEntry(title,'page'+str(page),0)
 y=H-126
def text(value,caption=False):
 global y
 p=Paragraph(value,small if caption else style);_,h=p.wrap(W-76,700)
 assert y-h>42,('Page overflow',page,value)
 p.drawOn(c,38,y-h);y-=h+9
def heading(value):
 global y
 y-=5;c.setFillColor(TEAL);c.setFont('Helvetica-Bold',13);c.drawString(38,y-13,value);y-=25
def image(name,maxheight,width=W-76):
 global y
 reader=ImageReader(str(IMG/name));iw,ih=reader.getSize();scale=min(width/iw,maxheight/ih);w,h=iw*scale,ih*scale
 assert y-h>42,('Image overflow',page,name)
 c.drawImage(reader,(W-w)/2,y-h,w,h,mask='auto');y-=h+10
def end():c.showPage()

start('Your cameras, your flight','Illustrated user manual - installation, everyday controls and airport views')
image('editor.png',450)
text('Build a camera library for each aircraft, adjust views in the simulator, and generate reusable airport cameras from scenery. This guide covers version 0.3 and its default controls.')
text('<b>Inside:</b> Setup 2 · Camera editing 3 · Movement 4 · Bindings 5 · Motion and behavior 6 · Airport creation 7 · Arrival and mini panel 8 · Profiles 9 · Updates 10 · Sharing and A320 starters 11 · Motion and panels 12 · Airport defaults 13 · Controller bindings 14 · Import and adapt 15.')
text('Images are captured from the release UI in a browser test environment. Example aircraft, values and airport selections illustrate the controls; they are not evidence of a live simulator test.',True)
end()

start('Install and start flying','Windows 11 x64 - Microsoft Flight Simulator 2024')
heading('1  Install the tester package')
text('Close MSFS. Run the supplied <b>Setup.exe</b> with your normal Windows account. Confirm the detected Community or Community2024 folder, or browse to your simulator package location. Select the parent Community folder, not the fs-camera-studio folder.')
text('The installer includes the panel, camera module and this manual. No SDK, Node, Python or background service is required. It refuses to replace a package while MSFS is running, or through an Addons Linker/development junction. Keep only one active copy.')
text('This tester installer is unsigned. Check its source and the supplied SHA-256 checksum before running it. Do not disable Windows security protections.')
heading('2  Open Camera Studio')
text('Start MSFS, load a parked flight and open Camera Studio from the top toolbar. Select a profile for the current aircraft. Select a camera and click <b>View in sim</b> or <b>Enable camera</b>. A live indicator shows which view is active.')
heading('3  Make a first adjustment')
text('Hold the right mouse button over the simulator scene and drag. Move with W/A/S/D, use Q to raise and E to lower the camera, and hold Shift for faster movement. Scroll the mouse wheel to zoom. Press <b>Ctrl+S</b> to save the adjusted active view and pending changes.')
heading('4  Return to normal simulator control')
text('<b>Compact</b> closes the editor and keeps quick access available. <b>Release camera</b> hands camera control back to MSFS. Closing the editor alone keeps the custom camera active.')
heading('First-flight checks')
text('Confirm that cockpit switches remain clickable, the MSFS top toolbar appears on hover, and right-drag stays smooth. Camera directions are configured automatically; no axis calibration is required. Airport world views and physical head-tracking devices still need in-sim testing on your configuration.')
end()

start('Edit a camera','Cameras > Position & Lens')
image('editor.png',450)
text('<b>Name and icon:</b> type directly in the camera-name box; the button beside it opens the icon picker. Color and category-default controls sit below the name. Use the library search to find views.')
text('<b>Position:</b> positive X moves right, positive Y moves up, and positive Z moves forward. Click directional buttons or type values in meters. Fine / Normal / Coarse controls adjustment precision and keyboard movement scale.')
text('<b>Orientation:</b> drag an arc handle or type pitch, yaw and roll in degrees. Each axis has a reset button. Field of view controls framing; smaller values zoom in. Save changes to persist edits.')
end()

start('Move, look and save','Movement follows the camera view, including its pitch')
heading('Default movement keys')
# Vector keyboard diagram, kept sharp at any zoom.
top=y;keys=[('W','Forward',140,0),('A','Left',80,-63),('S','Back',140,-63),('D','Right',200,-63),('Q','Up',330,0),('E','Down',390,0)]
for key,label,x,dy in keys:
 c.setFillColor(NAVY);c.roundRect(x,top+dy-42,48,40,5,fill=1,stroke=0);c.setFillColor(colors.white);c.setFont('Helvetica-Bold',18);c.drawCentredString(x+24,top+dy-28,key)
 c.setFillColor(GRAY);c.setFont('Helvetica',9);c.drawCentredString(x+24,top+dy-54,label)
y=top-136
text('<b>W/S</b> travel forward/back along the view; <b>A/D</b> strafe. <b>Q</b> raises and <b>E</b> lowers the camera vertically. Hold <b>Shift</b> for faster movement. Settings > Move speed sets the base speed; Fine uses 0.1x, Normal 1x and Coarse 10x.')
heading('Right-drag mouse look')
text('Hold right-click and drag over the simulator scene. The Mouse look button opens sensitivity, smoothing and per-camera options. A larger smoothing value eases the movement more; sensitivity changes how far the view turns for the same drag.')
text('<b>Keep look:</b> retain the adjusted direction after releasing the button.<br/><b>Return to saved:</b> return to the camera\'s saved direction after release.<br/><b>Remember temporary mouse look:</b> choose whether the offset is remembered when revisiting the camera. This is separate from the release behavior.')
heading('Save the view you are seeing')
text('<b>Ctrl+S</b> saves the adjusted active view, including temporary position/look/zoom, and commits pending edits. The mini-panel <b>Save all</b> button provides the same workflow. Use <b>Save current view to this camera</b> when editing a specific camera, then save pending changes.')
text('The normal Save changes action persists the edited draft; use Ctrl+S or the current-view capture control when you want temporary movement baked into the saved pose. Recalling a camera without saving can restore its saved position.')
heading('Avoid conflicting bindings')
text('All movement, zoom, save and tracking actions can be reassigned in Bindings. Existing custom assignments are preserved, so an upgraded profile may not receive a default key if that key is already used. Avoid assigning the same key to an MSFS aircraft action and a Camera Studio movement action.')
end()

start('Make bindings your own','Bindings - search, filter, assign and test')
image('bindings.png',340)
text('Use search to find an action, camera name or key. Filter by assigned/unassigned or keyboard/controller, and expand or collapse groups to keep the list manageable. Groups start collapsed; Expand all / Collapse all reflects the groups currently shown.')
heading('Assign or replace a shortcut')
text('Choose <b>Primary</b> or <b>Secondary</b> for an action, then press a key combination. Each slot works independently: keep a keyboard shortcut and controller input together. Review any conflict and choose Replace existing assignment to move that input. Clear assignment removes only the selected slot.')
text('The shortcut button beside a camera\'s name or its right-click <b>Assign key binding</b> action opens the binding dialog. A View ID is a binding slot, not a physical camera number: positive IDs identify views and 0 means unassigned. The previous 20-slot restriction has been removed; duplicate IDs are not valid.')
text('Actions include movement, fast movement, zoom, Save views, Toggle head tracking and Toggle auto airport camera switching. Check the displayed assignments after an upgrade: old custom shortcuts take precedence over new defaults.')
end()

start('Motion and behavior','Choose how each view moves and reacts')
image('behavior.png',310)
heading('Behavior belongs to the camera')
text('Set the view type and View ID, the default view for the profile, quick-access visibility and temporary-look policy. A <b>category default</b> is separate: selecting that category in the mini panel starts with its default camera.')
heading('Head tracking')
text('Head tracking is global and off by default. Its button or binding toggles the MSFS TrackIR-compatible channel for every camera and profile. Hardware and drivers must already work in MSFS; this does not provide a tracking driver or control independent VR systems.')
heading('Transitions and effects live under Motion')
text('Choose Instant, Smooth or Cinematic transitions and set duration/delay. Flight effects add temporary cockpit movement driven by telemetry. Start with Gentle and increase individual effects gradually. Effects do not overwrite your saved pose. Test interaction with any other camera-motion add-on.')
text('Some modes and capabilities are marked Preview or unavailable until qualified. Respect the in-app status: the presence of an option is not confirmation that your simulator build or aircraft supports it.',True)
end()

start('Generate airport cameras','Camera library + > Create airport cameras')
image('airports.png',440)
text('<b>1.</b> Select a nearby airport or enter ICAO codes separated by spaces. <b>2.</b> Choose Runways, Gates & apron, Taxi signs / intersections and/or Tower. <b>3.</b> Set camera height and horizontal offset, then click Preview cameras.')
text('Review the groups, select cameras, then <b>Save selected views</b>. Airport libraries are global, shared by every aircraft. Preview alone does not save them. To regenerate, choose replacement and confirm the warning to discard that airport\'s generated views and pending edits. Other airports and aircraft views remain unchanged.')
text('Scenery facility data supplies runway, parking and tower geometry. When actual sign positions are unavailable, the generator uses offset taxiway intersections. Accessible airport source XML can supply taxi signs; compiled or encrypted BGL files cannot be pasted into the XML field. Verify terrain clearance in the sim.')
end()

start('Arrive at a saved airport','Automatic airport loading and optional viewpoint switching')
heading('Generated and saved once - loaded on approach')
text('Saved airport cameras load automatically as you enter the configured radius, <b>25 NM by default</b>. Only one airport loads alongside the aircraft profile. Its camera groups appear under Airport in the library and mini panel. You do not need to regenerate them for every flight.')
text('Automatic selection uses the nearest saved airport and changes airports when another one is at least <b>2 NM closer</b>. A manually selected nearby airport stays selected while it remains in range. Use Automatic nearest to return to automatic selection. Airports outside the radius are unloaded from the current list, not deleted.')
image('airport-controls-mini-detail.png',125)
text('Browser-demo mini panel at release width. The short dock sits at the bottom and uses available width. Star cameras to show them here; camera lists scroll while category access remains available. Labeled cameras do not repeat their name in a tooltip.',True)
heading('Track the aircraft or keep a fixed view')
text('Airport cameras remain fixed in world space. Move them with the editor or W/A/S/D and Q/E, then save. <b>Track</b> chooses fixed direction, aircraft heading only, or heading and pitch toward the aircraft. Aircraft tracking is global, defaults to heading and pitch, and applies to airport and exterior views. Cockpit views never track the aircraft. Turn tracking off when setting a fixed composition. Camera and profile changes preserve both global tracking choices.')
heading('Auto switch changes the active viewpoint')
text('Enable <b>Auto switch</b> from the main header, mini toolbar or its binding. It immediately selects the nearest camera in the loaded airport set, even from a cockpit view, then selects nearer viewpoints as the aircraft moves. A minimum dwell and distance margin reduce rapid switching. Auto switch is shared across all cameras and categories. Manual camera selection, movement and right-drag pause switching for three seconds; the toggle stays on. Releasing camera control pauses switching until a camera is active again. Turn the shared toggle off to stop automatic selection.')
text('Automatic airport loading and Auto switch are separate features: entering the radius makes cameras available; it does not pull you out of your cockpit view. Explicitly enabling Auto switch can move you from the cockpit to an airport camera; simply arriving at an airport does not.')
end()

start('Organize and protect your views','Profiles, categories and backups')
image('profiles.png',265)
heading('Aircraft profiles')
text('The profile selector shows the current aircraft/profile. Profiles offers New profile, Save as new, Rename, Import, Export and Backups. <b>Save as new</b> is the duplicate-as-new workflow. Export a copy of valuable profiles before testing a new build.')
heading('Camera and category actions')
text('Right-click a camera or category, or use its three-dot menu, for rename, icon, color and organization actions. Cameras can inherit category color. A category-default camera is recalled when selecting its category in the mini panel. The star controls mini-panel visibility: only starred cameras appear there. Category headers and indented camera rows make the main library hierarchy clear; shortcut badges stay on the same row.')
text('Drag handles reorder the library. Move up/down and Move to category provide an alternative to dragging. Category expansion is a UI preference and should remain stable while editing. Settings > Appearance > Mini panel category display chooses icons, labels or both for mini categories only. The main library always shows category icons and labels. Camera buttons keep their names. Mini background opacity changes the background, not the controls; hover or keyboard-focus controls for tooltips.')
heading('Where data is kept')
text('MSFS stores profiles, airport cameras, settings and bindings separately from the installed Community package. Setup/uninstall preserves that storage. Package upgrade backups are under %LOCALAPPDATA%/FS Camera Studio/Backups. Package backups and exported profiles serve different purposes; retain both when testing.')
end()

start('Updates and troubleshooting','Version in the app bar - installer-assisted updates')
image('updates.png',175)
text('Click the version in the app bar to open <b>Version & updates</b>. Check for updates queries this project\'s public GitHub releases. Tester builds include newer tester releases. Automatic checks run once when the editor opens and can be disabled.')
text('When an update is found, Download update opens the installer URL in your browser. A selectable link is also shown if browser opening is unavailable. Save your work, close MSFS, and run the installer. Files are never replaced by the running panel. This browser handoff still needs in-sim confirmation for your simulator build.')
heading('If something does not work')
text('<b>No panel:</b> confirm the toolbar item is enabled and there is only one installed package. Restart MSFS after installation.<br/><b>No saved airport:</b> confirm Save selected views completed, the aircraft is within the radius, and the selected airport has saved cameras.<br/><b>No movement:</b> enable camera control, move focus out of text fields, and check bindings and conflicting MSFS assignments.<br/><b>No update result:</b> an HTTP error or timeout is not an up-to-date result. Retry later or open the release page.')
text('<b>Jerky look / blocked clicks / crash:</b> report the exact build, aircraft, airport, scenery add-ons, MSFS version and steps. State whether it occurred during flight loading or after a specific action. Include whether it persists after Release camera and with panels hidden.')
heading('Uninstall or roll back')
text('Close MSFS and uninstall through Windows Settings > Apps. To roll back, restore the whole fs-camera-studio folder from a package backup to Community, with only one copy active. Saved MSFS profile data is retained.')
text('<link href="https://github.com/Devinci297/FS_Camera_Studio/releases" color="#087a70">Official releases: github.com/Devinci297/FS_Camera_Studio/releases</link><br/>Offline checks do not replace live flight, scenery and hardware verification.',True)
end()

start('Share profiles and fly an A320','Export a selected profile - keep aircraft-specific refinements')
image('export-picker.png',210)
heading('Export the profile you want')
text('Open <b>Profiles > Export profile</b> and choose a profile from the list. <b>Export saved</b> uses its last saved version. For the active profile with pending edits, <b>Export with edits</b> includes those changes in the exported copy. Exporting another profile leaves your current camera open.')
text('Save the exported profile code as a JSON file. In MSFS, open <b>Import profile</b> and copy the displayed imports-folder path into Windows Explorer. Drop the file there, click <b>Refresh</b>, select it and click <b>Import</b>. MSFS cannot open a native file picker from this panel. Pasting profile code remains available as a fallback. The imported profile is saved as a separate copy with new IDs; existing profiles are preserved. Open it from the report or the profile list. For another aircraft, use <b>Copy for current aircraft</b> in the import report or <b>Copy to aircraft</b> on the profile card. The source is preserved; the new copy matches the loaded aircraft. Adjust positions for its cockpit geometry. Shared airport views remain separate.')
heading('A320 starter views across aircraft variants')
text('The built-in A320 starter recognizes the default/iniBuilds A320neo, FlyByWire A32NX and Fenix A320. Views are anchored to each model\'s pilot eyepoint, and each aircraft model keeps its own profile. An existing user profile is not replaced.')
text('<b>Starter framing, not calibrated closeups:</b> cockpit geometry differs between variants. Inspect and adjust instrument, overhead and pedestal views for the model you fly, then save. These are adaptable starting views, not a promise that every panel is framed identically in every A320 add-on.')
end()
start('Motion views and panels','Orbit, animated passes and quick access')
heading('Orbit, fly-by and walkaround')
text('Open the camera-library <b>+</b> menu and choose <b>External motion views</b>. This adds an orbit, a fly-by and six walkaround viewpoints. Existing cameras are preserved. Adjust the starting views for your aircraft and save.')
text('<b>Orbit:</b> circles your aircraft. Use the Motion tab to set radius, height, duration and direction.<br/><b>Fly-by:</b> animates a camera pass even while the aircraft is parked. Set pass distance, side offset, height and duration on the Motion tab. Duration controls speed. Repeat sends the camera back and forth; recalling the view restarts its pass.')
text('<b>Walkaround:</b> choose an exterior-camera category, seconds at each view and whether to repeat. Start or stop from the Walkaround button. The sequence follows category order and waits for transitions. Manual movement pauses the timer; selecting another camera or releasing control stops the sequence.')
heading('Panel visibility and adjustment')
text('Settings > Appearance contains independent <b>Main background opacity</b> and <b>Mini background opacity</b> sliders from 0 to 100 percent. Native simulator background blur is disabled so the scene remains clear through the panel; text stays opaque. The main editor always displays icons and labels; the mini display preference changes categories only. Airport categories use runway, gate, apron, taxiway and tower icons.')
text('Hold a position adjustment button to repeat at the selected precision. Release the button to stop. Losing focus also stops repeated adjustment. Mouse-look options open in a compact menu near the control.')
heading('Save feedback and current limits')
text('A successful save requests the simulator confirmation chime. Use <b>Settings > Save confirmation > Test sound</b> to check it. Spoken Cameras saved audio is available in the browser preview; MSFS does not support that recording in this panel. Check simulator UI volume if the chime is silent.')
text('This release passed automated native and packaged-UI checks. Motion views, opacity, folder enumeration and sound still require in-simulator verification across supported aircraft and systems.',True)
end()
start('Airport defaults and overrides','Settings > Tracking and Airport defaults')
image('airport-settings.png',325)
text('<b>Global tracking:</b> head tracking starts off; aircraft tracking starts at Heading + pitch. These choices persist across camera and profile changes. Cockpit cameras are excluded from aircraft tracking. Airport libraries belong to the airport, independently of the loaded aircraft profile.')
text('<b>Terrain-aware pitch:</b> Keep distant views near the horizon is on by default. It softens steep downward aiming toward a distant aircraft below the camera, while preserving heading. It helps with terrain differences; it cannot remove intervening scenery or make an obstructed aircraft visible.')
text('<b>Auto zoom:</b> off by default. Enable it to follow aircraft distance. Framing size controls space around the aircraft; minimum/maximum FOV bound magnification; smoothing time eases changes. Tracking must be enabled. Right-drag pauses automatic framing; switch Auto zoom off for manual zoom.')
text('<b>Category heights:</b> Runways, Gates, Apron and Taxiway intersections default to 3 m above their scenery feature; Tower defaults to 35 m above airport elevation. Runway approaches use at least 15 m and overviews 25 m. Save to persist changes.')
text('<b>Individual cameras:</b> open Behavior > Airport camera settings. Choose an Override for tracking, horizon protection or auto zoom. Turn off Global beside a numeric field to customize height or zoom. Use global settings for everything restores inheritance. Save changes or Ctrl+S commits the choices.')
end()

start('Keyboard and controller together','Primary / Secondary - button and hat capture')
image('controller-hat.png',330)
text('<b>1.</b> Open a camera binding from its shortcut button or right-click menu, or choose an action under Bindings. Select Primary or Secondary. A keyboard shortcut in the other slot is retained.')
text('<b>2.</b> Open the Controller button and select your device from the clickable list. Click <b>Listen for button or hat</b>. Press and release a normal button, or move the hat in one direction and return it to center. The detected input appears before you assign it.')
text('<b>3.</b> Click <b>Assign</b>, or review and confirm replacement if another action owns that input. Save your changes. Physical buttons and keyboard shortcuts trigger the same action independently.')
text('<b>Manual fallback:</b> expand Manual button or hat switch, choose the input and direction, then Test input. Stop listening before setting a different input. If no event arrives, check the selected controller and competing simulator assignments, then retry.')
text('This screenshot uses a demo T.16000M input. Native capture and both panel dialogs have automated coverage; physical T.16000M hat capture still needs live confirmation on the tester machine. Report the exact build, controller name, direction and whether normal buttons work before and after the hat attempt.',True)
end()
start('Import and adapt profiles','Bring a JSON file to any loaded aircraft')
image('import-profile.png',360)
text('<b>In MSFS:</b> open Profiles > Import profile. Copy the imports-folder path, paste it into Windows File Explorer, and place your Camera Studio JSON file there. Return to the panel, click Refresh, select the filename, then Import. Pasting profile code is also supported.')
text('<b>In the browser preview:</b> Choose profile file opens the normal browser file picker. The simulator panel does not provide a Windows file picker or an Open folder action. The folder list above uses sample filenames to show the simulator workflow.')
text('<b>Use another aircraft\'s views:</b> load the target aircraft and import the profile. Choose Copy for current aircraft in the import report, or Copy to aircraft from its profile card. Give the copy a name and choose Create copy. The original and existing target profiles are kept.')
text('Camera positions, zoom and behavior provide a starting point. Aircraft origins and cockpit sizes differ: adjust each view, then save. Airport cameras remain shared separately and are not included in aircraft-profile export.')
end()
c.save();print(OUT)
