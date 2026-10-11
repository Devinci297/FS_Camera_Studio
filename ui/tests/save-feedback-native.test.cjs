const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const source=fs.readFileSync('ui/save-feedback.js','utf8');
function harness(overrides={}) {
 const timers=new Map(),calls=[],registrations=[];let serial=0,ready;
 const window={RegisterCommBusListener(){},setTimeout(fn){timers.set(++serial,fn);return serial;},clearTimeout(id){timers.delete(id);},
   RegisterViewListener(...args){registrations.push(args);ready=args[1];return listener;},
   Audio(){throw new Error('Native must not attempt browser audio');},...overrides};
 const listener={call(...args){calls.push(args);return Promise.resolve(17);}};
 vm.runInNewContext(source,{window});
 return {window,listener,calls,registrations,timers,f:window.FSCSSaveFeedback,ready(){ready();},expire(){for(const fn of [...timers.values()])fn();}};
}
(async()=>{
 const h=harness(),f=h.f;
 assert.equal(f.isNative(),true);
 const pending=f.play('save-1');await Promise.resolve();
 assert.equal(h.calls.length,0,'No sound call before listener registration completes');
 assert.equal(h.registrations[0][0],'JS_LISTENER_SOUND');assert.equal(h.registrations[0][2],true);
 h.ready();assert.equal((await pending).state,'requested');
 await f.play('save-1');assert.deepEqual(h.calls,[['PLAY_SOUND','UI_VALID']]);
 await f.test();await f.test();assert.equal(h.calls.length,3);assert.equal(h.registrations.length,1);
 h.listener.call=()=>Promise.reject(new Error('Disconnected'));
 assert.equal((await f.play('save-2')).state,'unavailable');assert.match(f.getStatus().message,/still saved/);
 h.listener.call=()=>{throw new Error('Unavailable');};assert.equal((await f.test()).state,'unavailable');
 assert.equal(h.timers.size,0,'Completed requests clear their timers');
 const stalled=harness(),stalledPlay=stalled.f.test();stalled.expire();
 assert.equal((await stalledPlay).state,'unavailable');stalled.ready();await Promise.resolve();await Promise.resolve();
 assert.equal(stalled.calls.length,0,'Timed-out requests must not play a delayed chime');
 assert.equal((await stalled.f.test()).state,'requested','A later request can use the now-ready listener');
 const rejected=harness({RegisterViewListener(){throw new Error('No service');}});
 assert.equal((await rejected.f.test()).state,'unavailable');
 const missing=harness({RegisterViewListener:undefined});assert.equal((await missing.f.test()).state,'unavailable');
 const synchronous=harness({RegisterViewListener(name,ready){ready();return {call(){return Promise.resolve();}};}});
 assert.equal((await synchronous.f.test()).state,'requested','Synchronous ready callback is safe');
 const hung=harness();hung.listener.call=()=>new Promise(()=>{});const hungPlay=hung.f.test();hung.ready();
 for(let i=0;i<8;i++)await Promise.resolve();hung.expire();assert.equal((await hungPlay).state,'unavailable');
 console.log('Native sound listener readiness, dispatch, timeout, deduplication and failure isolation passed.');
})().catch(e=>{console.error(e);process.exitCode=1;});
