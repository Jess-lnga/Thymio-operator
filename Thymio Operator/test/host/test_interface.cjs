// node test/host/test_interface.cjs (aucune dependance npm)
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
async function test() {
  const source = fs.readFileSync(path.join(__dirname, '../../src/interface_wifi.cpp'), 'utf8');
  const js = source.split('<script>')[1].split('</script>')[0];
  const elements = new Map(), timers = new Map();
  let timerId = 0, hanging = false, staleResolve = null, stale = false;
  const flush = () => new Promise(resolve => setImmediate(resolve));
  class Element {
    constructor() { this.value = ''; this.disabled = false; }
    set id(id) { elements.set(id, this); }
    append() {}
    set innerHTML(html) {
      for (const m of html.matchAll(/id="([^"]+)"/g)) {
        const e = new Element(); e.id = m[1];
      }
    }
  }
  for (const id of ['phase','positions','catch','release','fields','save','cancel',
      'message','sliders','values','form','main','settings','mainTab','settingsTab']) {
    const e = new Element(); e.id = id;
  }
  let state = { phase:'Catch', busy:true, canCatch:false, canRelease:false,
    positions:[2030,2000], settings:[2400,2030,1440,1530,2000,10,8,8,10,10,10,300] };
  const context = vm.createContext({
    document: {getElementById:id=>elements.get(id), createElement:()=>new Element(),
      activeElement:null, addEventListener(){}},
    URLSearchParams, AbortController,
    setTimeout(fn, delay) { const id = ++timerId; timers.set(id,{fn,delay}); return id; },
    clearTimeout(id) { timers.delete(id); },
    fetch: async (url, options) => {
      if (stale && url === '/api/state') {
        stale = false;
        return new Promise(resolve => { staleResolve = resolve; });
      }
      if (hanging) return new Promise((resolve,reject) => {
        options.signal.addEventListener('abort',()=>reject(Error('aborted')));
      });
      return {ok:true, json:async()=>structuredClone(state)};
    }
  });
  async function timer(delay) {
    const entry = [...timers].find(([,t])=>t.delay===delay);
    assert.ok(entry, 'timer '+delay+' ms missing');
    timers.delete(entry[0]); entry[1].fn(); await flush();
  }
  vm.runInContext(js, context); await flush();
  assert.equal(elements.get('release').disabled,true);
  // Fin de Catch : disponible au prochain rafraichissement 100 ms.
  state = {...state, busy:false, canRelease:true};
  await timer(100); assert.equal(elements.get('release').disabled,false);
  // Une requete qui ne termine jamais doit expirer et etre retentee.
  hanging = true; await timer(250);
  await timer(2000); assert.equal(elements.get('release').disabled,true);
  hanging = false; await timer(250);
  assert.equal(elements.get('release').disabled,false);
  // Ancienne lecture arrivee apres une commande : ne pas restaurer l'ancien etat.
  stale = true; await timer(250);
  const old = structuredClone(state);
  state = {...state, phase:'Release', busy:true, canRelease:false};
  await elements.get('release').onclick(); await flush();
  staleResolve({ok:true,json:async()=>old}); await flush();
  assert.equal(elements.get('release').disabled,true);
  await timer(0);
  state = {...state, phase:'HOME', busy:false, canCatch:true};
  await timer(100); assert.equal(elements.get('catch').disabled,false);
  console.log('PASS interface: completion refresh, stalled request recovery, stale response rejected');
}
module.exports = test;
if (require.main === module) test().catch(error=>{console.error(error);process.exitCode=1;});
