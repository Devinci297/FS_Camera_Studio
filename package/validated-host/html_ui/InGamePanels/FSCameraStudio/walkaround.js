/* Aircraft inspection planning. Geometry is supplied by the read-only aircraft
 * study, never inferred from an aircraft name or a fixed set of camera offsets. */
(function(root){
  'use strict';
  const cross=(a,b,c)=>(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
  const distance=(a,b)=>Math.hypot(a[0]-b[0],a[1]-b[1]);
  function hull(points){
    const p=points.slice().sort((a,b)=>a[0]-b[0]||a[1]-b[1]).filter((v,i,a)=>!i||distance(v,a[i-1])>1e-7);
    const side=a=>{const out=[];for(const v of a){while(out.length>1&&cross(out[out.length-2],out[out.length-1],v)<=1e-8)out.pop();out.push(v);}return out;};
    return side(p).slice(0,-1).concat(side(p.slice().reverse()).slice(0,-1));
  }
  const inside=(point,poly,strict=false)=>poly.every((v,i)=>cross(v,poly[(i+1)%poly.length],point)>(strict?1e-7:-1e-7));
  function intersection(a,b,c,d){
    const ux=b[0]-a[0],uy=b[1]-a[1],vx=d[0]-c[0],vy=d[1]-c[1],den=ux*vy-uy*vx;
    if(Math.abs(den)<1e-9)return null;
    const t=((c[0]-a[0])*vy-(c[1]-a[1])*vx)/den,u=((c[0]-a[0])*uy-(c[1]-a[1])*ux)/den;
    return t>=-1e-8&&t<=1+1e-8&&u>=-1e-8&&u<=1+1e-8?Math.max(0,Math.min(1,t)):null;
  }
  function envelope(polygons,clearance){
    // Minkowski sum with a clearance square, then union. This preserves the
    // recesses ahead of/behind wings where the pilot inspects fuselage and gear.
    const expanded=polygons.map(poly=>hull(poly.flatMap(p=>[[-1,-1],[-1,1],[1,-1],[1,1]].map(s=>[p[0]+s[0]*clearance,p[1]+s[1]*clearance]))));
    const edges=[],key=p=>p.map(v=>Math.round(v*1e6)).join(',');
    expanded.forEach((poly,index)=>poly.forEach((a,i)=>{
      const b=poly[(i+1)%poly.length],ts=[0,1];
      expanded.forEach((other,j)=>{if(index===j)return;other.forEach((c,k)=>{const d=other[(k+1)%other.length],t=intersection(a,b,c,d);if(t!==null)ts.push(t);if(Math.abs(cross(a,b,c))<1e-7){const t=((c[0]-a[0])*(b[0]-a[0])+(c[1]-a[1])*(b[1]-a[1]))/distance(a,b)**2;if(t>0&&t<1)ts.push(t);}});});
      const point=t=>[a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t];ts.sort((a,b)=>a-b);
      for(let k=1;k<ts.length;k++)if(ts[k]-ts[k-1]>1e-8){
        const mid=point((ts[k]+ts[k-1])/2);
        if(!expanded.some((p,j)=>j!==index&&inside(mid,p,true)))edges.push([point(ts[k-1]),point(ts[k])]);
      }
    }));
    const unique=new Map(edges.map(e=>[key(e[0])+'|'+key(e[1]),e])),remaining=[...unique.values()],loops=[];
    while(remaining.length){const edge=remaining.pop(),loop=[edge[0]],start=key(edge[0]);let end=edge[1];
      while(key(end)!==start){loop.push(end);const next=remaining.findIndex(e=>key(e[0])===key(end));if(next<0)throw Error('Aircraft outline could not be connected.');end=remaining.splice(next,1)[0][1];if(loop.length>200)throw Error('Aircraft outline is too complex.');}
      // Intersections of overlapping, nearly coincident meshes can leave
      // micrometer edges. Remove them before native perimeter validation.
      for(let i=loop.length-1;i>=0&&loop.length>3;i--)if(distance(loop[i],loop[(i+1)%loop.length])<.002)loop.splice(i,1);
      loops.push(loop.filter((p,i,a)=>Math.abs(cross(a[(i+a.length-1)%a.length],p,a[(i+1)%a.length]))>1e-7));
    }
    // Interior holes between adjoining components are not walking loops.
    const outer=loops.filter(loop=>loop.reduce((sum,p,i)=>{const q=loop[(i+1)%loop.length];return sum+p[0]*q[1]-p[1]*q[0];},0)>1e-6);
    if(outer.length!==1||outer[0].length<3||outer[0].length>256)throw Error('Aircraft study needs a single connected footprint.');
    return outer[0];
  }
  function lookup(aircraft,catalog){
    if(!aircraft||!aircraft.modelKey)return null;
    if(aircraft.geometry?.acfProperties){
      const study=globalThis.FSCAircraftStudy.fromAcf(aircraft.geometry.acfProperties);
      return {...study,modelKey:aircraft.modelKey,variantKey:aircraft.variantKey||'',fingerprint:'acf-v1'};
    }
    return (catalog||{})[aircraft.modelKey.toLowerCase()+'|'+(aircraft.variantKey||'').toLowerCase()]||null;
  }
  function generate(study,{clearance=2,eyeHeight=1.7}={}){
    if(!study||study.version!==1||!Array.isArray(study.polygons)||!study.polygons.length||!Array.isArray(study.landmarks))throw Error('Study the loaded aircraft before generating an inspection.');
    if(!Number.isFinite(clearance)||clearance<.75||clearance>10||!Number.isFinite(eyeHeight)||eyeHeight<1||eyeHeight>2.2)throw Error('Choose clearance 0.75–10 m and eye height 1–2.2 m.');
    const finite=p=>Array.isArray(p)&&p.length===3&&p.every(v=>Number.isFinite(v)&&Math.abs(v)<1000);
    // Component count is independent of the final walking perimeter. Detailed
    // aircraft commonly contain more than 32 bodies and wing sections.
    if(study.polygons.length>256||study.polygons.reduce((sum,p)=>sum+(Array.isArray(p)?p.length:0),0)>8192)throw Error('Aircraft geometry exceeds the study limit (256 sections / 8192 vertices).');
    if(study.polygons.some(p=>!Array.isArray(p)||p.length<3||p.length>256||!p.every(finite)))throw Error('Aircraft geometry contains an invalid section or coordinate.');
    if(study.landmarks.length>60||!study.landmarks.length)throw Error('Aircraft study needs between 1 and 60 inspection points.');
    if(!Number.isFinite(study.groundDown)||!Number.isFinite(study.groundSlope))throw Error('Aircraft study is missing a valid ground height or attitude.');
    // Reserve 25 cm for the native walker's rounded turns (maximum 20 cm).
    const outline=envelope(study.polygons,clearance+.25),z=p=>study.groundDown+p[0]*study.groundSlope-eyeHeight;
    const perimeter=outline.map(p=>[p[0],p[1],z(p)]);
    const stops=study.landmarks.map((landmark,index)=>{
      if(!finite(landmark.point)||!Array.isArray(landmark.direction)||landmark.direction.length!==2||!landmark.direction.every(Number.isFinite)||Math.hypot(...landmark.direction)<.1)throw Error('Invalid inspection point.');
      const p=landmark.point,dir=landmark.direction,n=Math.hypot(...dir),end=[p[0]+dir[0]/n*3000,p[1]+dir[1]/n*3000];
      // Furthest exit along the chosen inspection direction avoids a camera
      // placed inside a wing/engine while still aiming at the actual component.
      let exit=null,furthest=-1;
      outline.forEach((a,i)=>{const t=intersection(p,end,a,outline[(i+1)%outline.length]);if(t!==null&&t>furthest){furthest=t;exit=[p[0]+(end[0]-p[0])*t,p[1]+(end[1]-p[1])*t];}});
      if(!exit)throw Error('Inspection point is outside the studied footprint.');
      const entry=[exit[0],exit[1],z(exit)];
      if(landmark.kind==='gear'&&Array.isArray(study.obstacles)){
        // Follow a straight access branch from the outer loop. Only parts low
        // enough to meet the walker block it; tall wings can be walked under.
        const unit=dir.map(v=>v/n),outer=distance(p,exit);
        let stand=Math.max(clearance+.25,1.25);
        for(const obstacle of study.obstacles){
          if(!Number.isFinite(obstacle.maxDown)||!Array.isArray(obstacle.polygon)||obstacle.polygon.length<3||!obstacle.polygon.every(finite))throw Error('Invalid aircraft obstacle.');
          const highestHead=Math.min(z(p),z(entry))-.25;
          if(obstacle.maxDown<highestHead)continue;
          const expanded=hull(obstacle.polygon.flatMap(v=>[[-1,-1],[-1,1],[1,-1],[1,1]].map(q=>[v[0]+q[0]*(clearance+.25),v[1]+q[1]*(clearance+.25)])));
          expanded.forEach((a,i)=>{const t=intersection(p,end,a,expanded[(i+1)%expanded.length]);if(t!==null)stand=Math.max(stand,t*3000);});
        }
        if(stand<outer-.05)exit=[p[0]+unit[0]*stand,p[1]+unit[1]*stand];
      }
      const position=[exit[0],exit[1],z(exit)],d=p.map((v,i)=>v-position[i]);
      const heading=Math.atan2(d[1],d[0])*180/Math.PI,pitch=Math.atan2(-d[2],Math.hypot(d[0],d[1]))*180/Math.PI;
      return {name:landmark.name,number:index+1,kind:landmark.kind,target:p,position,entry,heading,pitch};
    });
    return {perimeter,stops,clearance,eyeHeight};
  }
  const api={hull,envelope,lookup,generate};root.FSCWalkaround=api;
  if(typeof module!=='undefined')module.exports=api;
})(typeof globalThis!=='undefined'?globalThis:window);
