/* Read-only aircraft study. No simulator writes and no model-name size guesses.
 * Builds a local catalog of configuration geometry; encrypted/incomplete models
 * are reported, never substituted with a generic aircraft. Node 18+, no packages.
 */
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const {ini,study}=require('../ui/msfs-study.js');
function scan(roots) {
  const catalog={},report=[],seen=new Set(),ambiguous=new Set();
  function walk(dir) {
    let real;try{real=fs.realpathSync(dir);if(seen.has(real))return;seen.add(real);}catch{return;}
    let entries;try{entries=fs.readdirSync(dir,{withFileTypes:true});}catch{return;}
    for(const entry of entries) {
      const file=path.join(dir,entry.name);
      if(entry.isDirectory()||entry.isSymbolicLink()){walk(file);continue;}
      if(entry.name.toLowerCase()!=='flight_model.cfg')continue;
      const normalized=file.replaceAll('\\','/'),match=normalized.match(/\/SimObjects\/Airplanes\/([^/]+)\/(.*)flight_model\.cfg$/i);
      if(!match||/\/attachments\//i.test(normalized)||/\/common\//i.test(normalized))continue;
      const key=match[1].toLowerCase()+'|'+match[2].replace(/\/$/,'').toLowerCase();
      try {
        const modelRoot=normalized.slice(0,normalized.toLowerCase().indexOf('/simobjects/airplanes/')+('/SimObjects/Airplanes/'+match[1]).length);
        const base=path.join(modelRoot,'common/config/flight_model.cfg');
        const read=p=>{try{return fs.readFileSync(p,'utf8');}catch{return '';}};
        const common=read(base),local=read(file),engineText=read(path.join(modelRoot,'common/config/engines.cfg'))+'\n'+read(path.join(dir,'engines.cfg'));
        const config={...ini(common),...ini(local)};
        // Modular attachments override physical geometry and require their own
        // transform resolution. Refuse a partial study instead of misplacing wheels.
        const aircraft=read(path.join(dir,'aircraft.cfg'));
        if(/\[SIM_ATTACHMENT\./i.test(aircraft))throw Error('Modular attachment transforms need analysis');
        const result=study(config,ini(engineText));result.modelKey=match[1].toLowerCase();result.variantKey=match[2].replace(/\/$/,'').toLowerCase();
        result.fingerprint=crypto.createHash('sha256').update(common+local+engineText).digest('hex');
        if(catalog[key]&&catalog[key].fingerprint!==result.fingerprint)ambiguous.add(key);
        if(ambiguous.has(key))throw Error('Ambiguous duplicate aircraft identity');
        require('../ui/walkaround.js').generate(result);
        catalog[key]=result;report.push({key,status:'studied',stops:result.landmarks.length});
      }catch(e){delete catalog[key];report.push({key,file,status:'unavailable',reason:e.message});}
    }
  }
  roots.forEach(walk);return {catalog,report};
}
function defaultRoots() {
  const app=process.env.APPDATA||'',local=process.env.LOCALAPPDATA||'';
  const cfgs=[path.join(app,'Microsoft Flight Simulator 2024/UserCfg.opt'),path.join(local,'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/UserCfg.opt')];
  for(const file of cfgs)try{const match=fs.readFileSync(file,'utf8').match(/^InstalledPackagesPath\s+"([^"]+)"/m);if(match)return ['Community','Community2024','Official2020','Official2024','StreamedPackages'].map(n=>path.join(match[1],n));}catch{}
  return [];
}
if(require.main===module) {
  const args=process.argv.slice(2),at=args.indexOf('--output'),output=at<0?null:args.splice(at,2)[1];
  const reportAt=args.indexOf('--report'),reportFile=reportAt<0?output+'.report.json':args.splice(reportAt,2)[1];
  if(!output)throw Error('Usage: node tools/study-aircraft.cjs --output <catalog.js> [package roots...]');
  const result=scan(args.length?args:defaultRoots());fs.mkdirSync(path.dirname(output),{recursive:true});
  fs.writeFileSync(output,'/* Generated local aircraft studies. */\nglobalThis.FSCAircraftFootprints = '+JSON.stringify(result.catalog)+';\n');
  fs.mkdirSync(path.dirname(reportFile),{recursive:true});fs.writeFileSync(reportFile,JSON.stringify(result.report,null,2)+'\n');
  console.log('Studied '+Object.keys(result.catalog).length+' aircraft variants; '+result.report.filter(r=>r.status==='unavailable').length+' unavailable. '+output);
}
module.exports={ini,study,scan};
