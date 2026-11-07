// Synthetic geometry for UI workflow tests, independent of installed aircraft.
module.exports=page=>page.evaluate(()=>{
 const aircraft=FSCApp.state.aircraft;
 window.FSCAircraftFootprints={[aircraft.modelKey.toLowerCase()+'|'+(aircraft.variantKey||'').toLowerCase()]:{
  version:1,source:'Synthetic inspection test',modelKey:aircraft.modelKey,variantKey:aircraft.variantKey||'',
  lengthMeters:20,spanMeters:20,groundDown:3,groundSlope:0,
  polygons:[[[10,-1,0],[10,1,0],[-10,1,0],[-10,-1,0]],[[2,-10,0],[2,10,0],[-2,10,0],[-2,-10,0]]],
  landmarks:[['Nose',[10,0,0],[1,0]],['Right wing',[0,10,0],[0,1]],['Right aft',[-8,1,0],[0,1]],['Tail',[-10,0,0],[-1,0]],['Left aft',[-8,-1,0],[0,-1]],['Left wing',[0,-10,0],[0,-1]]].map(([name,point,direction])=>({name,point,direction}))
 }};
});
