/* Read-only Plane Maker geometry -> the shared inspection planner's meters FRD.
 * Body meshes are part-local; wing meshes are already in aircraft coordinates.
 * No aircraft-name heuristics. The flight-model envelope is not a scenery mesh.
 */
(function(root){
  'use strict';
  const W=typeof module!=='undefined'?require('./walkaround.js'):root.FSCWalkaround,FT=.3048;
  function parseAcf(text){
    const p={};
    for(const line of text.split(/\r?\n/)){const m=line.match(/^P (\S+) (.*)$/);if(m&&Number.isFinite(Number(m[2])))p[m[1]]=Number(m[2]);}
    return p;
  }
  function fromAcf(p){
    const n=(k,fallback)=>{const v=p[k]??fallback;if(!Number.isFinite(v))throw Error('Missing ACF geometry: '+k);return v;};
    const cg=[n('acf/_cgX',0),n('acf/_cgY'),n('acf/_cgZ')];
    const frd=v=>[(cg[2]-v[2])*FT,(v[0]-cg[0])*FT,(cg[1]-v[1])*FT];
    const polygons=[],bodies=[],wings=[],landmarks=[],obstacles=[];
    const groups=[...new Set(Object.keys(p).map(k=>k.match(/^(_(?:body|wing)\/\d+)\//)?.[1]).filter(Boolean))];
    const rotate=(v,base)=>{
      // Rotate in FRD, then return to Plane Maker's right/up/aft frame.
      let [x,y,z]=[-v[2],v[0],-v[1]];
      const roll=n(base+'/_part_phi',0)*Math.PI/180,pitch=n(base+'/_part_the',0)*Math.PI/180,yaw=n(base+'/_part_psi',0)*Math.PI/180;
      [y,z]=[y*Math.cos(roll)-z*Math.sin(roll),y*Math.sin(roll)+z*Math.cos(roll)];
      [x,z]=[x*Math.cos(pitch)+z*Math.sin(pitch),-x*Math.sin(pitch)+z*Math.cos(pitch)];
      [x,y]=[x*Math.cos(yaw)-y*Math.sin(yaw),x*Math.sin(yaw)+y*Math.cos(yaw)];
      return [y+n(base+'/_part_x',0),-z+n(base+'/_part_y',0),-x+n(base+'/_part_z',0)];
    };
    for(const base of groups){
      if(!n(base+'/_part_specs_eq',0))continue;
      const points=[],body=base.startsWith('_body/'),s=n(base+'/_s_dim',0),r=n(base+'/_r_dim',0);
      if(s<1||s>100||r<1||r>100)continue;
      for(let i=0;i<s;i++)for(let j=0;j<r;j++){
        const v=[0,1,2].map(k=>p[base+'/_geo_xyz/'+i+','+j+','+k]);
        if(v.every(Number.isFinite))points.push(frd(body?rotate(v,base):v));
      }
      if(points.length<3)continue;
      let poly=W.hull(points);
      // Drop almost-collinear mesh detail without reducing clearance materially.
      while(poly.length>32){let at=0,min=Infinity;poly.forEach((b,i)=>{const a=poly[(i+poly.length-1)%poly.length],c=poly[(i+1)%poly.length];const area=Math.abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]));if(area<min){min=area;at=i;}});poly.splice(at,1);}
      const area=Math.abs(poly.reduce((sum,a,i)=>{const b=poly[(i+1)%poly.length];return sum+a[0]*b[1]-a[1]*b[0];},0)/2);
      if(poly.length<3||area<.005)continue;
      polygons.push(poly);(body?bodies:wings).push({points,poly,area});
      obstacles.push({polygon:poly,maxDown:Math.max(...points.map(p=>p[2]))});
    }
    const rotors=[];
    for(let i=0;i<n('acf/_num_prop',0);i++){
      const base='_blad/'+i,radius=n(base+'/_semilen_SEG',0);
      if(radius<=0)continue;
      const disk=[];
      for(let k=0;k<16;k++){
        const angle=k*Math.PI/8,r=radius/Math.cos(Math.PI/16);
        const point=frd(rotate([r*Math.cos(angle),r*Math.sin(angle),0],base));
        // A vertical propeller has a line footprint; include a thin hub slab.
        for(const f of [-.05,.05])for(const r of [-.05,.05])disk.push([point[0]+f,point[1]+r,point[2]]);
      }
      const polygon=W.hull(disk);polygons.push(polygon);obstacles.push({polygon,maxDown:Math.max(...disk.map(p=>p[2]))});
      rotors.push({point:frd(rotate([0,0,0],base)),name:(Math.abs(n(base+'/_part_the',0))>45?'Rotor ':'Propeller ')+(i+1)});
    }
    if(!bodies.length)throw Error('ACF has no usable body footprint.');
    // The longest physical body is the fuselage; engine nacelles remain obstacles.
    const extent=b=>Math.max(...b.points.map(p=>p[0]))-Math.min(...b.points.map(p=>p[0]));
    const fus=bodies.sort((a,b)=>extent(b)-extent(a))[0],nose=fus.points.reduce((a,b)=>a[0]>b[0]?a:b),tail=fus.points.reduce((a,b)=>a[0]<b[0]?a:b);
    const center=fus.points.reduce((a,b)=>a.map((v,i)=>v+b[i]/fus.points.length),[0,0,0]),length=nose[0]-tail[0];
    const add=(name,point,direction,kind='airframe')=>landmarks.push({name,point,direction,kind});
    const sidePoint=(fraction,side)=>{const f=nose[0]-length*fraction;const near=fus.points.filter(v=>Math.abs(v[0]-f)<length*.08);return (near.length?near:fus.points).reduce((a,b)=>side*b[1]>side*a[1]?b:a);};
    const gear=[];
    for(let i=0;i<20;i++){
      const b='_gear/'+i;if(!n(b+'/_gear_type',0))continue;
      const len=n(b+'/_leg_len',0),lat=n(b+'/_latE',0)*Math.PI/180,lon=n(b+'/_lonE',0)*Math.PI/180;
      const point=frd([n(b+'/_gear_x')+len*Math.sin(lat),n(b+'/_gear_y')-len*Math.cos(lat)*Math.cos(lon),n(b+'/_gear_z')+len*Math.sin(lon)]);
      gear.push({index:i,point});
      const radius=Math.max(.15,n(b+'/_tire_radius',0)*FT);
      obstacles.push({polygon:[[-radius,-radius],[-radius,radius],[radius,radius],[radius,-radius]].map(v=>[point[0]+v[0],point[1]+v[1],point[2]]),maxDown:point[2]+radius});
    }
    if(!gear.length)throw Error('ACF has no usable landing-gear positions.');
    const engines=[];for(let i=0;i<n('acf/_num_engn',0);i++){const b='_engn/'+i;engines.push({index:i,point:frd([n(b+'/_part_x'),n(b+'/_part_y'),n(b+'/_part_z')])});}
    add('Left forward fuselage',sidePoint(.2,-1),[0,-1]);add('Nose',nose,[1,0]);
    const front=gear.filter(g=>g.point[0]>0&&Math.abs(g.point[1]-center[1])<1);
    front.forEach(g=>add('Forward landing gear '+(g.index+1),g.point,[0,1],'gear'));
    const wingStops=side=>{
      const candidates=wings.filter(w=>w.points.some(v=>side*v[1]>1));
      if(!candidates.length)return;
      const main=candidates.sort((a,b)=>b.area-a.area)[0];
      const selected=candidates.filter(w=>Math.abs((Math.max(...w.points.map(v=>v[0]))+Math.min(...w.points.map(v=>v[0])))/2-(Math.max(...main.points.map(v=>v[0]))+Math.min(...main.points.map(v=>v[0])))/2)<length*.3);
      const points=selected.flatMap(w=>w.points).filter(v=>side*v[1]>.5),tip=points.reduce((a,b)=>side*a[1]>side*b[1]?a:b);
      const outer=points.filter(v=>side*v[1]>side*tip[1]*.65),lead=outer.reduce((a,b)=>a[0]>b[0]?a:b),trail=outer.reduce((a,b)=>a[0]<b[0]?a:b);
      return {tip,lead,trail,center:points.reduce((a,b)=>a.map((v,i)=>v+b[i]/points.length),[0,0,0])};
    };
    const engineStops=side=>{
      engines.filter(e=>side===1?e.point[1]>=0:e.point[1]<0).forEach(e=>add((side>0?'Right':'Left')+' engine '+(e.index+1),e.point,[1,side],'engine'));
      rotors.filter(e=>side===1?e.point[1]>=0:e.point[1]<0).forEach(e=>add(e.name,e.point,[1,side],'engine'));
    };
    add('Right forward fuselage',sidePoint(.2,1),[0,1]);add('Right center fuselage',sidePoint(.4,1),[0,1]);
    const right=wingStops(1),left=wingStops(-1);
    if(right)add('Right center wing',right.center,[1,1]);engineStops(1);
    if(right){add('Right wing leading edge',right.lead,[1,1]);add('Right wing tip',right.tip,[0,1]);add('Right wing trailing edge',right.trail,[-1,1]);}
    const rest=gear.filter(g=>!front.includes(g));rest.filter(g=>g.point[1]>=0).forEach(g=>add('Landing gear '+(g.index+1),g.point,[0,1],'gear'));
    add('Right aft fuselage',sidePoint(.8,1),[0,1]);add('Tail',tail,[-1,0]);add('Left aft fuselage',sidePoint(.8,-1),[0,-1]);
    rest.filter(g=>g.point[1]<0).forEach(g=>add('Landing gear '+(g.index+1),g.point,[0,-1],'gear'));
    if(left){add('Left wing trailing edge',left.trail,[-1,-1]);add('Left wing tip',left.tip,[0,-1]);add('Left wing leading edge',left.lead,[1,-1]);}engineStops(-1);
    const pitch=n('acf/_the_eqlbm',0)*Math.PI/180,all=polygons.flat();
    return {version:1,source:'X-Plane ACF flight-model geometry',polygons,obstacles,landmarks,groundDown:n('acf/_h_eqlbm')*FT/Math.cos(pitch),groundSlope:Math.tan(pitch),lengthMeters:length,spanMeters:Math.max(...all.map(v=>v[1]))-Math.min(...all.map(v=>v[1]))};
  }
  const api={parseAcf,fromAcf};root.FSCAircraftStudy=api;if(typeof module!=='undefined')module.exports=api;
})(typeof globalThis!=='undefined'?globalThis:window);
