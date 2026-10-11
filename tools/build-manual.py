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
 c.setFillColor(GRAY);c.setFont('Helvetica',8);c.drawString(38,23,'Tester build 0.1.1-test.20261011.2  |  11 October 2026');c.drawRightString(W-38,23,str(page))
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
text('Build a camera library for each aircraft, adjust views in the simulator, and generate reusable airport cameras from scenery. This guide covers the tester release and its default controls.')
text('<b>Inside:</b> Setup 2 · Camera editing 3 · Movement 4 · Bindings 5 · Motion and behavior 6 · Airport creation 7 · Arrival and mini panel 8 · Profiles 9 · Updates and troubleshooting 10.')
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
text('Hold the right mouse button over the simulator scene and drag. Move with W/A/S/D, use Q/E for height, and hold Shift for faster movement. Scroll the mouse wheel to zoom. Press <b>Ctrl+S</b> to save the adjusted active view and pending changes.')
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
top=y;keys=[('W','Forward',140,0),('A','Left',80,-63),('S','Back',140,-63),('D','Right',200,-63),('Q','Down',330,0),('E','Up',390,0)]
for key,label,x,dy in keys:
 c.setFillColor(NAVY);c.roundRect(x,top+dy-42,48,40,5,fill=1,stroke=0);c.setFillColor(colors.white);c.setFont('Helvetica-Bold',18);c.drawCentredString(x+24,top+dy-28,key)
 c.setFillColor(GRAY);c.setFont('Helvetica',9);c.drawCentredString(x+24,top+dy-54,label)
y=top-136
text('<b>W/S</b> travel forward/back along the view; <b>A/D</b> strafe. <b>Q</b> lowers and <b>E</b> raises the camera vertically. Hold <b>Shift</b> for faster movement. Settings > Move speed sets the base speed; Fine uses 0.1x, Normal 1x and Coarse 10x.')
heading('Right-drag mouse look')
text('Hold right-click and drag over the simulator scene. The Mouse look button opens sensitivity, smoothing and per-camera options. A larger smoothing value eases the movement more; sensitivity changes how far the view turns for the same drag.')
text('<b>Keep look:</b> retain the adjusted direction after releasing the button.<br/><b>Return to saved:</b> return to the camera\'s saved direction after release.<br/><b>Remember temporary mouse look:</b> choose whether the offset is remembered when revisiting the camera. This is separate from the release behavior.')
heading('Save the view you are seeing')
text('<b>Ctrl+S</b> saves the adjusted active view, including temporary position/look/zoom, and commits pending edits. The mini-panel <b>Save</b> button provides the same workflow. Use <b>Save current view to this camera</b> when editing a specific camera, then save pending changes.')
text('The normal Save changes action persists the edited draft; use Ctrl+S or the current-view capture control when you want temporary movement baked into the saved pose. Recalling a camera without saving can restore its saved position.')
heading('Avoid conflicting bindings')
text('All movement, zoom, save and tracking actions can be reassigned in Bindings. Existing custom assignments are preserved, so an upgraded profile may not receive a default key if that key is already used. Avoid assigning the same key to an MSFS aircraft action and a Camera Studio movement action.')
end()

start('Make bindings your own','Bindings - search, filter, assign and test')
image('bindings.png',340)
text('Use search to find an action, camera name or key. Filter by assigned/unassigned or keyboard/controller, and expand or collapse groups to keep the list manageable.')
heading('Assign or replace a shortcut')
text('Click <b>Assign</b>, then press a key combination. The dialog also supports controller inputs and a test action. If the input is already assigned, review the conflict and use the provided clear/replace action. <b>Clear</b> removes the selected assignment.')
text('The shortcut button beside a camera\'s name opens its binding directly. A View ID is a binding slot, not a physical camera number: positive IDs identify views and 0 means unassigned. The previous 20-slot restriction has been removed; duplicate IDs are not valid.')
text('Actions include movement, fast movement, zoom, Save views, Toggle head tracking and Toggle auto airport camera switching. Check the displayed assignments after an upgrade: old custom shortcuts take precedence over new defaults.')
end()

start('Motion and behavior','Choose how each view moves and reacts')
image('behavior.png',310)
heading('Behavior belongs to the camera')
text('Set the view type and View ID, the default view for the profile, quick-access visibility and temporary-look policy. A <b>category default</b> is separate: selecting that category in the mini panel starts with its default camera.')
heading('Head tracking')
text('Use the Head tracking button or its binding to toggle the simulator\'s TrackIR-compatible channel for this camera. Behavior offers explicit on/off or simulator-controlled behavior. Hardware and its drivers must already work in MSFS; this does not provide a tracking driver or control independent VR systems.')
heading('Transitions and effects live under Motion')
text('Choose Instant, Smooth or Cinematic transitions and set duration/delay. Flight effects add temporary cockpit movement driven by telemetry. Start with Gentle and increase individual effects gradually. Effects do not overwrite your saved pose. Test interaction with any other camera-motion add-on.')
text('Some modes and capabilities are marked Preview or unavailable until qualified. Respect the in-app status: the presence of an option is not confirmation that your simulator build or aircraft supports it.',True)
end()

start('Generate airport cameras','Camera library + > Create airport cameras')
image('airports.png',440)
text('<b>1.</b> Select a nearby airport or enter ICAO codes separated by spaces. <b>2.</b> Choose Runways, Gates & apron, Taxi signs / intersections and/or Tower. <b>3.</b> Set camera height and horizontal offset, then click Preview cameras.')
text('Review the generated groups, select the cameras to keep, then <b>Save selected views</b>. Preview alone does not save them. Regeneration preserves refinements unless you explicitly choose to replace matching generated views.')
text('Scenery facility data supplies runway, parking and tower geometry. When actual sign positions are unavailable, the generator uses offset taxiway intersections. Accessible airport source XML can supply taxi signs; compiled or encrypted BGL files cannot be pasted into the XML field. Verify terrain clearance in the sim.')
end()

start('Arrive at a saved airport','Automatic airport loading and optional viewpoint switching')
heading('Generated and saved once - loaded on approach')
text('Saved airport cameras load automatically as you enter the configured radius, <b>25 NM by default</b>. Only one airport loads alongside the aircraft profile. Its camera groups appear under Airport in the library and mini panel. You do not need to regenerate them for every flight.')
text('Automatic selection uses the nearest saved airport and changes airports when another one is at least <b>2 NM closer</b>. A manually selected nearby airport stays selected while it remains in range. Use Automatic nearest to return to automatic selection. Airports outside the radius are unloaded from the current list, not deleted.')
image('airport-controls-mini-detail.png',125)
text('Mini-panel UI example with a long camera list. The camera area and toolbar scroll independently instead of growing to cover the scene.',True)
heading('Track the aircraft or keep a fixed view')
text('Airport cameras remain fixed in world space. Move them with the editor or W/A/S/D and Q/E, then save. <b>Track</b> chooses fixed direction, aircraft heading only, or heading and pitch toward the aircraft. Turn tracking off when setting a fixed composition.')
heading('Auto switch changes the active viewpoint')
text('While viewing an airport camera, enable <b>Auto switch</b> in the mini toolbar or use its binding. It selects nearer airport viewpoints as the aircraft moves, with a minimum dwell and distance margin to reduce rapid switching. Manual camera selection, movement or right-drag turns Auto switch off.')
text('Automatic airport loading and Auto switch are separate features: entering the radius makes cameras available; it does not pull you out of your cockpit view. Auto switch only operates while an airport view is active.')
end()

start('Organize and protect your views','Profiles, categories and backups')
image('profiles.png',330)
heading('Aircraft profiles')
text('The profile selector shows the current aircraft/profile. Profiles offers New profile, Save as new, Rename, Import, Export and Backups. <b>Save as new</b> is the duplicate-as-new workflow. Export a copy of valuable profiles before testing a new build.')
heading('Camera and category actions')
text('Right-click a camera or category, or use its three-dot menu, for rename, icon, color and organization actions. Cameras can inherit category color. A category-default camera is recalled when selecting its category in the mini panel. The star controls quick access.')
text('Drag handles reorder the library. Move up/down and Move to category provide an alternative to dragging. Category expansion is a UI preference and should remain stable while editing. Use Settings to choose icons, labels or both and select an appearance.')
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
text('<link href="https://github.com/Devinci297/FS_Camera_Studio/releases" color="#087a70">Official releases: github.com/Devinci297/FS_Camera_Studio/releases</link><br/>This is a tester build. Offline checks do not replace live flight, scenery and hardware verification.',True)
end()
c.save();print(OUT)
