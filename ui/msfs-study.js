/* MSFS configuration geometry reader; see doc/walkaround.md. */
(function(root){
'use strict';
const FT=.3048;
function ini(text) {
  const out={};let section='';
  for(const raw of text.split(/\r?\n/)) {
    const line=raw.split(';')[0].trim(),head=line.match(/^\[([^\]]+)\]/);
    if(head){section=head[1].toLowerCase();continue;}
    const at=line.indexOf('=');if(at<0)continue;
    out[section+'.'+line.slice(0,at).trim().toLowerCase()]=line.slice(at+1).trim().replace(/^"|"$/g,'');
  }return out;
}
function study(config,engines={}) {
  const num=(key)=>{if(!(key in config))throw Error('Missing '+key);const n=Number(config[key]);if(!Number.isFinite(n))throw Error('Invalid '+key);return n;};
  const vec=(key,defaultValue)=>{
    const v=key in config?config[key].split(',').map(Number):defaultValue;
    if(!v||v.length!==3||!v.every(Number.isFinite))throw Error('Missing or invalid '+key);return v;
  };
  // All configured positions are longitudinal, lateral, vertical in feet from
  // datum. Convert to the visual SimObject origin, then canonical meters FRD.
  const datum=vec('weight_and_balance.reference_datum_position',[0,0,0]);
  const frd=(v)=>[(v[0]+datum[0])*FT,(v[1]+datum[1])*FT,-(v[2]+datum[2])*FT];
  const g='airplane_geometry.',center=vec(g+'fuselage_center_pos');
  const length=num(g+'fuselage_length'),width=num(g+'fuselage_diameter');
  if(length<=0||width<=0||width>=length||length>1200)throw Error('Invalid fuselage dimensions');
  const polygons=[],landmarks=[];
  const add=(name,points)=>{polygons.push(points.map(frd));};
  add('Fuselage',[[center[0]+length/2,center[1]-width/2,center[2]],[center[0]+length/2,center[1]+width/2,center[2]],[center[0]-length/2,center[1]+width/2,center[2]],[center[0]-length/2,center[1]-width/2,center[2]]]);
  const surface=(prefix)=>{
    const span=num(g+prefix+'_span'),sweep=num(g+prefix+'_sweep')*Math.PI/180;
    const root=prefix==='wing'?num(g+'wing_root_chord'):num(g+'htail_area')/span*2;
    const tipChord=prefix==='wing'?Math.max(0,2*num(g+'wing_area')/span-root):root;
    const taper=tipChord/root,mac=2*root/3*(1+taper+taper*taper)/(1+taper);
    const yMac=span/6*(1+2*taper)/(1+taper);
    // MSFS positions the quarter-MAC aerodynamic center relative to visual origin.
    // wing_pos_apex_lon is obsolete and is deliberately not used.
    const apex=prefix==='wing'?num('aerodynamics.aero_center_lift')-datum[0]+Math.tan(sweep)*yMac+mac/4:num(g+'htail_pos_lon');
    const up=num(g+prefix+(prefix==='wing'?'_pos_apex_vert':'_pos_vert'));
    if(span<=0||span>1200||root<=0||Math.abs(sweep)>1.4)throw Error('Invalid '+prefix+' geometry');
    // Full root chord at the tip is deliberately conservative: taper does not
    // shrink the walking envelope. Sweep and actual apex locate both edges.
    const tip=apex-Math.tan(sweep)*span/2;
    for(const side of [-1,1])add(prefix,[[apex,0,up],[tip,side*span/2,up],[tip-root,side*span/2,up],[apex-root,0,up]]);
    return {span,root,apex,up,tip};
  };
  const wing=surface('wing');if(Number(config[g+'htail_span'])>0)surface('htail');
  const contacts=[];
  for(const [key,value]of Object.entries(config))if(/^contact_points\.point\.\d+$/.test(key)) {
    let parts=value.split(',').map(s=>s.trim()),name='';
    // Named legacy tuple and 2024 hash-map entries are both accepted.
    if(value.includes('#')||/^Name:/i.test(value)) {
      const fields=Object.fromEntries(value.split('#').map(p=>{const i=p.indexOf(':');return[p.slice(0,i).trim().toLowerCase(),p.slice(i+1).trim()];}));
      if(!fields.properties)throw Error('Unsupported contact-point format');
      parts=fields.properties.split(',').map(s=>s.trim());name=fields.name||'';
    } else if(!Number.isFinite(Number(parts[0]))){name=parts.shift();}
    const values=parts.map(Number);if(values.length<4||!values.slice(0,4).every(Number.isFinite))throw Error('Invalid contact point');
    if(values[0]===1)contacts.push({name,point:frd(values.slice(1,4)),radius:Math.max(0,Number(values[6])||0)*FT});
  }
  if(!contacts.length)throw Error('No readable wheel contact positions');
  const pitch=num('contact_points.static_pitch')*Math.PI/180;
  if(Math.abs(pitch)>.35)throw Error('Unsupported parked attitude');
  const cg=frd(vec('weight_and_balance.empty_weight_cg_position'));
  const groundDown=cg[2]+num('contact_points.static_cg_height')*FT/Math.cos(pitch)-cg[0]*Math.tan(pitch);
  if(!Number.isFinite(groundDown)||Math.abs(groundDown)>100)throw Error('Invalid ground height');
  const landmark=(name,p,kind='airframe',direction)=>landmarks.push({name,point:frd(p),kind,direction});
  const sideF=(fraction,side)=>[center[0]+length*(.5-fraction),center[1]+side*width/2,center[2]];
  landmark('Left forward fuselage',sideF(.2,-1),'airframe',[0,-1]);
  landmark('Nose',[center[0]+length/2,center[1],center[2]],'airframe',[1,0]);
  const fore=contacts.filter(c=>c.point[0]>=Math.max(...contacts.map(c=>c.point[0]))-.5);
  const gear=(items,label,side)=>items.forEach((c,i)=>landmarks.push({name:c.name||label+(items.length>1?' '+(i+1):''),point:[c.point[0],c.point[1],c.point[2]-c.radius],kind:'gear',direction:[0,side]}));
  gear(fore,'Forward landing gear',1);
  landmark('Right forward fuselage',sideF(.2,1),'airframe',[0,1]);
  landmark('Right center fuselage',sideF(.4,1),'airframe',[0,1]);
  landmark('Right center wing',[wing.apex-wing.root/2,wing.span*.22,wing.up],'airframe',[1,1]);
  const enginePoints=Object.entries(engines).filter(([k])=>/^generalenginedata\.engine\.\d+$/.test(k)).map(([k,v])=>({index:Number(k.split('.').pop())+1,p:v.split(',').map(Number)}));
  if(enginePoints.some(e=>e.p.length!==3||!e.p.every(Number.isFinite)))throw Error('Invalid engine position');
  const engineStops=(side)=>enginePoints.filter(e=>side===1?e.p[1]>=0:e.p[1]<0).forEach(e=>landmark((side===1?'Right':'Left')+' engine '+e.index,e.p,'engine',[1,side]));
  engineStops(1);
  landmark('Right wing leading edge',[wing.tip,wing.span*.48,wing.up],'airframe',[1,1]);
  landmark('Right wing tip',[wing.tip-wing.root/2,wing.span/2,wing.up],'airframe',[0,1]);
  landmark('Right wing trailing edge',[wing.tip-wing.root,wing.span*.48,wing.up],'airframe',[-1,1]);
  const rear=contacts.filter(c=>!fore.includes(c));gear(rear.filter(c=>c.point[1]>=0),'Right main landing gear',1);
  landmark('Right aft fuselage',sideF(.8,1),'airframe',[0,1]);
  landmark('Tail / APU',[center[0]-length/2,center[1],center[2]],'airframe',[-1,0]);
  landmark('Left aft fuselage',sideF(.8,-1),'airframe',[0,-1]);
  gear(rear.filter(c=>c.point[1]<0),'Left main landing gear',-1);
  landmark('Left wing trailing edge',[wing.tip-wing.root,-wing.span*.48,wing.up],'airframe',[-1,-1]);
  landmark('Left wing tip',[wing.tip-wing.root/2,-wing.span/2,wing.up],'airframe',[0,-1]);
  landmark('Left wing leading edge',[wing.tip,-wing.span*.48,wing.up],'airframe',[1,-1]);
  engineStops(-1);
  // Thrust and wheel positions can lie outside the aerodynamic surfaces. Include
  // them in the nearest component envelope before placing any inspection stop.
  const hull=typeof module!=='undefined'?require('./walkaround.js').hull:root.FSCWalkaround.hull;
  for(const landmark of landmarks.filter(p=>p.kind==='engine'||p.kind==='gear')){
    let closest=0,distance=Infinity;
    polygons.forEach((poly,i)=>{const d=Math.min(...poly.map(p=>Math.hypot(p[0]-landmark.point[0],p[1]-landmark.point[1])));if(d<distance){distance=d;closest=i;}});
    polygons[closest]=hull([...polygons[closest],landmark.point]);
  }
  return {version:1,source:'Aircraft configuration envelope',polygons,landmarks,groundDown,groundSlope:Math.tan(pitch),lengthMeters:length*FT,spanMeters:wing.span*FT};
}

const api={ini,study};root.FSCMsfsStudy=api;if(typeof module!=='undefined')module.exports=api;
})(typeof globalThis!=='undefined'?globalThis:window);
