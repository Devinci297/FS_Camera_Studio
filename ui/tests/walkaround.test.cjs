const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const W=require('../walkaround.js'),A=require('../aircraft-study.js'),M=require('../msfs-study.js');
const rectangle=[[12,-1,0],[12,1,0],[-10,1,0],[-10,-1,0]];
const study={version:1,polygons:[rectangle,[[1,-1,0],[0,-12,0],[-3,-12,0],[-2,-1,0]],[[1,1,0],[0,12,0],[-3,12,0],[-2,1,0]]],landmarks:[{name:'Nose',point:[12,0,0],direction:[1,0]},{name:'Right gear',point:[-1,1,2],direction:[0,1],kind:'gear'}],groundDown:3,groundSlope:.05};
const plan=W.generate(study);
assert.ok(plan.perimeter.length>8);assert.equal(plan.stops.length,2);
for(const stop of plan.stops)assert.ok(Math.abs(stop.position[2]-(3+stop.position[0]*.05-1.7))<1e-8);
assert.throws(()=>W.generate({...study,groundDown:NaN}));assert.throws(()=>W.generate(study,{clearance:0}));
// Detailed ACF aircraft have many independently modelled bodies/wing sections.
// Retaining those sections must not reject an otherwise valid footprint.
const detailed={...study,polygons:study.polygons.concat(Array.from({length:40},(_,i)=>{const r=.2+i*.01;return [[-r,-r,0],[r,-r,0],[r,r,0],[-r,r,0]];}))};
assert.deepEqual(W.generate(detailed),plan);
assert.throws(()=>W.generate({...study,polygons:Array(257).fill(rectangle)}),/256 sections/);
assert.throws(()=>W.generate({...study,polygons:[[[NaN,0,0],[1,0,0],[0,1,0]]]}),/invalid section or coordinate/);
const gearStudy={...study,landmarks:[{name:'Gear',point:[-1,1,2.5],direction:[0,1],kind:'gear'}],obstacles:[{polygon:rectangle,maxDown:4},{polygon:study.polygons[2],maxDown:0}]};
const accessible=W.generate(gearStudy).stops[0];assert.ok(accessible.position[1]<accessible.entry[1]-5);
const blocked=W.generate({...gearStudy,obstacles:[{polygon:study.polygons[2],maxDown:4}]}).stops[0];assert.ok(blocked.position[1]>accessible.position[1]+5);
const cfg=M.ini(`[WEIGHT_AND_BALANCE]
reference_datum_position=10,0,2
empty_weight_cg_position=-10,0,0
[AIRPLANE_GEOMETRY]
fuselage_center_pos=-10,0,0
fuselage_length=30
fuselage_diameter=5
wing_span=36
wing_area=180
wing_root_chord=6
wing_sweep=0
wing_pos_apex_lon=999
wing_pos_apex_vert=2
htail_span=0
[AERODYNAMICS]
aero_center_lift=1
[CONTACT_POINTS]
static_pitch=0
static_cg_height=5
point.0=Name: nose #Properties: 1,0,0,-5,1500,0,1
point.1=1,-15,5,-5,1500,0,1
point.2=1,-15,-5,-5,1500,0,1
[GENERALENGINEDATA]
engine.0=0,0,1`);
const ms=M.study(cfg,cfg),mp=W.generate(ms);
assert.equal(ms.landmarks.filter(p=>p.kind==='gear').length,3);
assert.equal(ms.landmarks.filter(p=>p.kind==='engine').length,1);
assert.ok(Math.max(...ms.polygons[1].map(p=>p[0]))<2); // obsolete apex must have no effect
assert.equal(mp.stops.length,18);
// Optional real-aircraft verification is read-only and is not required in CI.
for(const file of process.argv.slice(2)){
 const acf=A.fromAcf(A.parseAcf(fs.readFileSync(file,'utf8'))),p=W.generate(acf);
 assert.ok(p.stops.length>=10);assert.ok(p.perimeter.length>=3);
 assert.ok(p.perimeter.every((v,i)=>Math.hypot(v[0]-p.perimeter[(i+1)%p.perimeter.length][0],v[1]-p.perimeter[(i+1)%p.perimeter.length][1])>=.001));
 assert.ok(acf.polygons.every(poly=>poly.every(v=>v.every(Number.isFinite))));
 console.log(path.basename(file),JSON.stringify({length:acf.lengthMeters,span:acf.spanMeters,stops:p.stops.length,gear:acf.landmarks.filter(p=>p.kind==='gear').length}));
}
console.log('Walkaround geometry tests passed.');
