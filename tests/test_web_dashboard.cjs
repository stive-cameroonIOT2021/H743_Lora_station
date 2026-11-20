/* Offline DOM behavior checks, without downloading browser dependencies.
 * Run: node --test tests/test_web_dashboard.cjs
 * Firmware compilation separately checks the real ESP-IDF HTTP/Wi-Fi APIs. */
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../main/web/index.html'), 'utf8');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];

class TextNode {
  constructor(value) { this.nodeType=3;this.nodeName='#text';this.nodeValue=String(value);this.parentNode=null; }
  get textContent() { return this.nodeValue; }
  set textContent(value) { this.nodeValue=String(value); }
  remove() { if(this.parentNode)this.parentNode.removeChild(this); }
}

class Element {
  constructor(tag = 'div') {
    this.tagName = tag; this.nodeType=1;this.nodeName=tag.toUpperCase();this._nodes = []; this.attributes = {}; this.events = {};
    this.dataset = new Proxy({}, {set:(target,key,value)=>{
      target[key]=String(value);this.attributes['data-'+String(key).replace(/[A-Z]/g,letter=>'-'+letter.toLowerCase())]=String(value);return true;
    }}); this.className = ''; this.disabled = false;
    this.style = {setProperty(name,value) { this[name]=String(value); },removeProperty(name) { delete this[name]; }};
    this.parentNode = null; this.hidden = false; this.open = false; this.value = '';
    this.textWrites = 0; this.replacements = 0;
    const classes = () => new Set(this.className.split(/\s+/).filter(Boolean));
    const update = values => { this.className=[...values].join(' '); };
    this.classList = {
      add:(...names)=>{ const values=classes(); names.forEach(name=>values.add(name)); update(values); },
      remove:(...names)=>{ const values=classes(); names.forEach(name=>values.delete(name)); update(values); },
      contains:name=>classes().has(name),
      toggle:(name,force)=>{const values=classes(),enabled=force??!values.has(name);enabled?values.add(name):values.delete(name);update(values);return enabled;}
    };
  }
  set className(value) { this.attributes.class=String(value); }
  get className() { return this.attributes.class??''; }
  set textContent(value) {
    this.textWrites++;this._nodes.forEach(child=>child.parentNode=null);this._nodes=[];
    if(String(value)!=='')this.append(new TextNode(value));
  }
  get textContent() { return this._nodes.map(child=>child.textContent).join(''); }
  append(...nodes) {
    for(let node of nodes) {
      if(!(node instanceof Element)&&!(node instanceof TextNode))node=new TextNode(node);
      node.remove();node.parentNode=this;this._nodes.push(node);
    }
  }
  appendChild(node) { this.append(node); return node; }
  replaceChildren(...nodes) { this.replacements++; this.textContent=''; this.append(...nodes); }
  removeChild(node) { const index=this._nodes.indexOf(node);if(index<0)throw new Error('Missing child');this._nodes.splice(index,1);node.parentNode=null;return node; }
  replaceChild(node,old) { const index=this._nodes.indexOf(old);if(index<0)throw new Error('Missing child');node.remove();this._nodes[index]=node;node.parentNode=this;old.parentNode=null;return old; }
  remove() { if(this.parentNode)this.parentNode.removeChild(this); }
  get children() { return this._nodes.filter(node=>node instanceof Element); }
  get firstChild() { return this._nodes[0]??null; }
  get lastChild() { return this._nodes.at(-1)??null; }
  get childNodes() { return this._nodes; }
  get options() { return this.children.filter(child=>child.tagName==='option'); }
  setAttribute(key, value) {
    value=String(value);this.attributes[key]=value;
    if(key==='class')this.className=value;
    if(key==='id')this.id=value;
    if(key==='value')this.value=value;
    if(key==='hidden')this.hidden=true;
    if(key==='open')this.open=true;
    if(key.startsWith('data-'))this.dataset[key.slice(5).replace(/-([a-z])/g,(_,letter)=>letter.toUpperCase())]=value;
  }
  getAttribute(key) { return this.attributes[key]??null; }
  getAttributeNames() { return Object.keys(this.attributes); }
  hasAttribute(key) { return Object.hasOwn(this.attributes,key); }
  removeAttribute(key) { delete this.attributes[key]; if(key==='hidden')this.hidden=false;if(key==='open')this.open=false; }
  addEventListener(name, callback) { this.events[name] = callback; }
  querySelectorAll(selector) { return descendants(this).filter(node=>matches(node,selector)); }
  querySelector(selector) { return this.querySelectorAll(selector)[0]??null; }
  focus() { this.focused = true; }
}

function descendants(node) { return node.children.flatMap(child=>child instanceof Element?[child,...descendants(child)]:[]); }
function matches(node,selector) {
  return selector.split(',').some(part=>{
    const match=part.trim().match(/^([\w-]+)?(?:#([\w-]+))?(?:\.([\w-]+))?(?:\[([\w-]+)(?:=["']?([^\]"']+)["']?)?\])?$/);
    if(!match)throw new Error(`Unsupported test selector: ${selector}`);
    return (!match[1]||node.tagName===match[1])&&(!match[2]||node.id===match[2])&&
      (!match[3]||node.classList.contains(match[3]))&&(!match[4]||(node.getAttribute(match[4])!==null&&(!match[5]||node.getAttribute(match[4])===match[5])));
  });
}
function staticBody() {
  const root=new Element('body'),stack=[root];
  const content=html.split('<body>')[1].split('<script>')[0];
  for(const token of content.matchAll(/<!--[\s\S]*?-->|<[^>]+>|[^<]+/g)) {
    const value=token[0];
    if(value.startsWith('<!--'))continue;
    if(value.startsWith('</')) { if(stack.length>1)stack.pop();continue; }
    if(value.startsWith('<')) {
      const tag=value.match(/^<([\w-]+)/)?.[1];if(!tag)continue;
      const node=new Element(tag);
      for(const attribute of value.matchAll(/([\w:-]+)=(?:"([^"]*)"|'([^']*)')/g))node.setAttribute(attribute[1],attribute[2]??attribute[3]);
      if(/\shidden(?:\s|>)/.test(value))node.hidden=true;
      stack.at(-1).append(node);
      if(!value.endsWith('/>')&&!['input','br','hr','meta','link','img'].includes(tag))stack.push(node);
    } else stack.at(-1).append(value);
  }
  return root;
}

function fixture() {
  const reading = fields => ({age_s:0,stale:false,...fields});
  return {
    station:{uptime_ms:5000,session:'ground-a',radio_ready:true,key_configured:true,protocol_version:2,cipher:'AES-256-GCM',nonce_ready:true,tx_nonces_used:2,uplink_state:'connected',last_rx_age_ms:0,expected_period_ms:5000,rssi_dbm:-80,snr_db:7.5,rx_missing:0,frequency_hz:433775000,tx_count:2,rx_auth_errors:0,usb_dropped:0,request_history:[{id:17,status:'done'}]},
    device:{terminal_print_enabled:true,led_gpio:21,free_heap_bytes:100000,ap_clients:1},
    telemetry:{session:'air-a',target:{system:1,component:1},mode:'STABILIZE',armed:false,link:{status:'connected',heartbeat_age_s:0},_radio_period_s:5,_air_ack_age_s:0,_air_rtt_ms:50,_air_link_status:'connected',elapsed_s:4,received_messages:23,bad_data_messages:0,
      attitude:reading({roll_deg:0,pitch_deg:1,yaw_deg:2}),position:reading({latitude_deg:0,longitude_deg:0,relative_altitude_m:0}),gps:reading({fix_type:1,fix_label:'no fix',satellites:0,hdop:null}),battery:reading({voltage_v:0,current_a:0,remaining_pct:null,load_pct:0}),motion:reading({climb_m_s:0,groundspeed_m_s:0}),rc:reading({channels:[1500,0,null],rssi_raw:null}),
      imus:[reading({id:0,acceleration_m_s2:[0,null,-9.8],gyro_rad_s:[0,0,0]})],pressures:[reading({id:0,absolute_hpa:1000})],vibration:reading({x_m_s2:0,clipping:[0,0,0]}),distance:null,optical_flow:null,
      outputs:reading({channels:[1000,null]}),esc:[reading({id:0,rpm:0,voltage_v:null})],health:reading({unhealthy:[],unhealthy_mask:0}),ekf:reading({flags:167}),power:reading({vcc_v:5}),memory:reading({free_bytes:10000}),mcu:reading({id:0,temperature_c:42}),local_position:null,navigation:reading({bearing_deg:0}),mission:reading({sequence:0,total:0}),home:null,batteries:[reading({id:0,cell_voltages_v:[4.1,null,0]})],
      status_text:[{severity:2,age_s:0,text:'<img src=x onerror=alert(1)> FC text'}],cache:{accepted_frames:1,invalid_frames:0},last_result:{request_id:17,status:0}}
  };
}

const LIVE_KEYS = ['session','target','mode','armed','link','_radio_period_s','_air_ack_age_s','_air_rtt_ms','_air_link_status',
  'elapsed_s','received_messages','bad_data_messages','cache','attitude','battery','gps','position','motion','rc','imus','status_text'];
function compact(value) {
  if(!value||!value.telemetry||!value.station||!value.device)return structuredClone(value);
  const result=structuredClone(value);
  result.telemetry=Object.fromEntries(LIVE_KEYS.filter(key=>Object.hasOwn(result.telemetry,key)).map(key=>[key,result.telemetry[key]]));
  delete result.station.request_history;delete result.station.recent_packets;delete result.station.frequency_hz;
  return result;
}

function harness() {
  const body=staticBody(), timeouts = new Map(), intervals = [], frames=new Map(), calls = [], replies = [], listeners = {};
  const elements={get:id=>descendants(body).find(node=>node.id===id)};
  let now = 100, nextTimer = 1, created=0, latestFull=null, autoDetails=true;
  const pendingLive=[],pendingDetails=[],detailReplies=[];
  const response=reply=>reply instanceof Error?Promise.reject(reply):Promise.resolve({ok:true,status:200,json:async()=>structuredClone(reply)});
  function settle(queue,reply) { const item=queue.shift();if(item){reply instanceof Error?item.reject(reply):item.resolve({ok:true,status:200,json:async()=>structuredClone(reply)});}return !!item; }
  async function flush() {
    await new Promise(resolve=>setImmediate(resolve));
    const scheduled=[...frames.values()];frames.clear();scheduled.forEach(callback=>callback(now));
    await new Promise(resolve=>setImmediate(resolve));
  }
  async function immediateTimers() {
    for(let round=0;round<4;round++) {
      const ready=[...timeouts].filter(([,entry])=>entry.delay===0);
      if(!ready.length)return;
      for(const [id,entry] of ready){timeouts.delete(id);entry.callback();}
      await flush();
    }
  }
  const document = {
    hidden:false,body,
    getElementById:id=>elements.get(id),
    createElement:tag=>{created++;return new Element(tag);},
    createElementNS:(_namespace,tag)=>{created++;return new Element(tag);},
    createTextNode:value=>new TextNode(value),
    querySelectorAll:selector=>body.querySelectorAll(selector),
    addEventListener:(name,callback)=>{ listeners[name]=callback; }
  };
  const fetch = (url,options) => {
    calls.push({url,options});
    if(url==='/api/telemetry') {
      if(detailReplies.length)return response(detailReplies.shift());
      if(autoDetails&&latestFull)return response(latestFull);
    } else if(replies.length) {
      const reply=replies.shift();return response(url==='/api/live'&&!(reply instanceof Error)?compact(reply):reply);
    }
    const queue=url==='/api/telemetry'?pendingDetails:pendingLive;
    return new Promise((resolve,reject)=>{
      const item={resolve,reject};queue.push(item);
      options.signal.addEventListener('abort',()=>{const index=queue.indexOf(item);if(index>=0)queue.splice(index,1);reject(new Error('aborted'));},{once:true});
    });
  };
  vm.runInNewContext(script,{document,fetch,AbortController,performance:{now:()=>now},Date:class extends Date{static now(){return 1700000000000+now;}},setTimeout:(callback,delay)=>{const id=nextTimer++;timeouts.set(id,{callback,delay});return id;},clearTimeout:id=>timeouts.delete(id),setInterval:callback=>intervals.push(callback),requestAnimationFrame:callback=>{const id=nextTimer++;frames.set(id,callback);return id;},cancelAnimationFrame:id=>frames.delete(id),console});
  return {
    elements,calls,replies,document,
    advance:ms=>{now+=ms;},
    tick:async()=>{intervals.forEach(callback=>callback());await flush();},
    timer:async delay=>{
      const entry=[...timeouts].find(([,value])=>value.delay===delay);
      assert.ok(entry,`Expected a ${delay} ms timeout`); timeouts.delete(entry[0]); entry[1].callback();
      await flush();
    },
    snapshot:async value=>{
      latestFull=structuredClone(value);
      if(!settle(pendingLive,compact(value))){
        replies.push(compact(value));
        const entry=[...timeouts].find(([,item])=>item.delay===0||item.delay===250);
        assert.ok(entry,'Expected the next fast live poll');timeouts.delete(entry[0]);entry[1].callback();
      }
      if(autoDetails)while(pendingDetails.length)settle(pendingDetails,latestFull);
      await flush();
    },
    click:async id=>{elements.get(id).events.click();await flush();await immediateTimers();},
    change:async(id,value)=>{const node=elements.get(id);node.value=String(value);node.events.change({target:node});await flush();},
    openRaw:async open=>{const node=elements.get('raw-details');node.open=open;node.events.toggle?.({target:node});await flush();await immediateTimers();},
    deferDetails:()=>{autoDetails=false;},
    full:async value=>{if(!settle(pendingDetails,value))detailReplies.push(structuredClone(value));await flush();},
    detailPending:()=>pendingDetails.length,
    livePending:()=>pendingLive.length,
    created:()=>created,
    visibility:async hidden=>{document.hidden=hidden;listeners.visibilitychange();await flush();},
    text:id=>{assert.ok(elements.get(id),`Missing rendered element #${id}`);return elements.get(id).textContent;},
    scheduled:()=>[...timeouts.values()]
  };
}

test('dashboard is offline and never inserts received text as HTML',()=>{
  assert.doesNotMatch(html,/<(?:script|link|img)[^>]*(?:src|href)=["']https?:/);
  assert.doesNotMatch(script,/\.innerHTML\s*=|insertAdjacentHTML|document\.write/);
  assert.equal((html.match(/role="tab"/g)||[]).length,6);
  new vm.Script(script);
});

test('startup waits independently; valid zero and unknown remain distinct',async()=>{
  const h=harness();
  assert.equal(h.text('browser-state'),'Connecting');
  assert.equal(h.text('radio-state'),'Waiting');
  await h.snapshot(fixture());
  assert.equal(h.text('browser-state'),'Connected');
  assert.equal(h.text('radio-state'),'Connected');
  assert.equal(h.text('fc-state'),'Connected');
  assert.equal(h.text('ack-state'),'Connected');
  assert.match(h.text('metrics'),/0\.00 V/);
  assert.match(h.text('metrics'),/--% remaining/);
  assert.match(h.text('metrics'),/-80\.0 dBm/);
  assert.match(h.text('records'),/1500 \/ 0 \/ --/);
  assert.match(h.text('records'),/<img src=x onerror=alert\(1\)> FC text/);
  assert.match(h.text('flight-state'),/DISARMED/);
});

test('every telemetry category and all transport metadata are accessible',async()=>{
  const h=harness(); await h.snapshot(fixture());
  for (const [tab,expected] of Object.entries({sensors:['IMU 0','Barometer 0','Vibration','Range sensor','Optical flow'],motors:['Actuator commands','RC inputs','ESC feedback 0'],health:['Sensor health','EKF estimator','FC power','Memory','MCU','Local N / E / D','Navigation','Mission','Home','Battery 0'],pipeline:['Ground station / radio','ESP32 device','Aircraft metadata / results','433775000','Request History']})) {
    await h.click(`tab-${tab}`);
    for (const value of expected) assert.ok(h.text('records').includes(value),`${tab}: missing ${value}`);
  }
  assert.match(h.text('records'),/accepted_frames/);
  assert.match(h.text('records'),/led|Led/);
  await h.openRaw(true);
  assert.match(h.text('raw-json'),/cell_voltages_v/);
});

test('browser outage retains data and advances source, radio and FC ages',async()=>{
  const h=harness(); await h.snapshot(fixture());
  h.advance(45000); h.replies.push(new Error('network unavailable')); await h.timer(250);
  assert.equal(h.text('browser-state'),'Connection lost');
  assert.equal(h.text('radio-state'),'Lost');
  assert.equal(h.text('fc-state'),'Lost');
  assert.equal(h.text('ack-state'),'Lost');
  assert.match(h.text('records'),/STALE 45\.0 s/);
  assert.match(h.text('metrics'),/0\.00 V/);
  assert.match(h.text('notice'),/Displaying the last snapshot/);
  assert.equal(h.elements.get('terminal-toggle').disabled,true);
});

test('hidden tab pauses; timeout and in-flight guards prevent overlapping polling',async()=>{
  const h=harness(); await h.timer(0);
  assert.equal(h.calls.length,1);
  assert.equal(h.scheduled().filter(item=>item.delay===250).length,0);
  await h.visibility(true);
  assert.equal(h.text('browser-state'),'Paused');
  assert.equal(h.scheduled().length,0);
  await h.visibility(false);
  h.replies.push(fixture()); await h.timer(0);
  assert.equal(h.calls.length,2);
  assert.equal(h.text('browser-state'),'Connected');
});

test('terminal toggle uses bounded JSON POST and leaves no flight command action',async()=>{
  const h=harness(); await h.snapshot(fixture());
  h.replies.push({terminal_print_enabled:false}); await h.click('terminal-toggle');
  const request=h.calls.at(-1);
  assert.equal(request.url,'/api/terminal');
  assert.equal(request.options.method,'POST');
  assert.deepEqual(JSON.parse(request.options.body),{enabled:false});
  assert.equal(request.options.headers['Content-Type'],'application/json');
  assert.equal(h.text('terminal-toggle'),'Prints off');
  assert.match(h.text('control-message'),/USB machine data continues/);
  assert.doesNotMatch(script,/\/api\/(?:command|arm|motor|set_key)/);
});

test('invalid snapshots cannot replace the last valid telemetry',async()=>{
  const h=harness(); await h.snapshot(fixture());
  h.replies.push({station:{},telemetry:null,device:{}}); await h.timer(250);
  assert.equal(h.text('flight-mode'),'STABILIZE');
  assert.equal(h.text('browser-state'),'Reconnecting');
  assert.match(h.text('metrics'),/0\.00 V/);
});

function chart(h,id) {
  const node=h.elements.get(id);
  assert.ok(node,`Missing chart #${id}`);
  return {
    node,
    samples:Number(node.getAttribute('data-samples')),
    segments:Number(node.getAttribute('data-segments')),
    latest:JSON.parse(node.getAttribute('data-latest')),
    lines:node.querySelectorAll('polyline')
  };
}

function sample(index,{voltage=12,current=2,age=0}={}) {
  const value=fixture();
  value.station.uptime_ms=5000+index*1000;
  value.telemetry.cache.accepted_frames=index+1;
  value.telemetry.elapsed_s=4+index;
  for(const reading of [value.telemetry.attitude,value.telemetry.battery,...value.telemetry.imus])reading.age_s=age;
  value.telemetry.battery.voltage_v=voltage;
  value.telemetry.battery.current_a=current;
  return value;
}

test('horizon uses the real roll and pitch, and compass wraps north without losing zero',async()=>{
  const h=harness(),value=fixture();
  value.telemetry.attitude={age_s:0,stale:false,roll_deg:15,pitch_deg:5,yaw_deg:-1};
  await h.snapshot(value);
  const horizon=h.elements.get('instrument-horizon');
  assert.equal(horizon.getAttribute('data-roll'),'15');
  assert.equal(horizon.getAttribute('data-pitch'),'5');
  assert.match(h.elements.get('horizon-scene').getAttribute('transform'),/rotate\(-15\)/);
  assert.match(h.elements.get('horizon-scene').getAttribute('transform'),/translate\(0[ ,]+12\.5\)/);
  assert.equal(h.elements.get('instrument-compass').getAttribute('data-heading'),'359');
  assert.match(h.text('compass-heading'),/359/);
  assert.match(h.elements.get('compass-rose').getAttribute('transform'),/rotate\(-359[ ,]/);
  value.telemetry.attitude={age_s:0,stale:false,roll_deg:0,pitch_deg:0,yaw_deg:0};
  h.advance(1000);value.station.uptime_ms+=1000;await h.snapshot(value);
  assert.equal(h.elements.get('instrument-horizon').getAttribute('data-roll'),'0');
  assert.equal(h.elements.get('instrument-compass').getAttribute('data-heading'),'0');
  assert.match(h.text('compass-heading'),/000/);
});

test('unavailable instruments never become invented level or north readings; stale is explicit',async()=>{
  const h=harness(),value=fixture();
  value.telemetry.attitude=null;
  await h.snapshot(value);
  assert.equal(h.elements.get('instrument-horizon').getAttribute('data-state'),'waiting');
  assert.equal(h.elements.get('instrument-compass').getAttribute('data-heading'),'');
  assert.match(h.text('compass-heading'),/---/);
  value.telemetry.attitude={age_s:0,stale:false,roll_deg:Infinity,pitch_deg:NaN,yaw_deg:Infinity};
  await h.snapshot(value);
  assert.equal(h.elements.get('instrument-horizon').getAttribute('data-roll'),'');
  assert.equal(h.elements.get('instrument-compass').getAttribute('data-heading'),'');
  value.telemetry.attitude={age_s:25,stale:true,roll_deg:10,pitch_deg:3,yaw_deg:45};
  await h.snapshot(value);
  assert.equal(h.elements.get('instrument-horizon').getAttribute('data-state'),'stale');
  assert.equal(h.elements.get('instrument-compass').getAttribute('data-state'),'stale');
  assert.match(h.text('horizon-state'),/STALE/i);
  assert.match(h.text('compass-state'),/STALE/i);
  const attributes=descendants(h.document.body).flatMap(node=>Object.values(node.attributes));
  assert.ok(attributes.every(attribute=>!/(?:NaN|Infinity)/.test(attribute)));
});

test('IMU plots select reported devices and convert radians per second to degrees per second',async()=>{
  const h=harness(),value=fixture();
  value.telemetry.imus=[
    {id:0,age_s:0,stale:false,acceleration_m_s2:[1,2,3],gyro_rad_s:[Math.PI/2,0,-Math.PI]},
    {id:1,age_s:0,stale:false,acceleration_m_s2:[4,5,6],gyro_rad_s:[0,Math.PI/4,0]}
  ];
  await h.snapshot(value);await h.click('tab-sensors');
  assert.deepEqual(chart(h,'imu-accel-plot').latest,[1,2,3]);
  const gyro=chart(h,'imu-gyro-plot').latest;
  assert.ok(Math.abs(gyro[0]-90)<1e-9);assert.equal(gyro[1],0);assert.ok(Math.abs(gyro[2]+180)<1e-9);
  assert.match(h.text('visuals'),/m\/s/);assert.match(h.text('visuals'),/deg\/s/);
  await h.change('imu-select','1');
  assert.deepEqual(chart(h,'imu-accel-plot').latest,[4,5,6]);
  assert.ok(Math.abs(chart(h,'imu-gyro-plot').latest[1]-45)<1e-9);
});

test('power charts calculate watts from reported voltage and current, retaining real zero',async()=>{
  const h=harness();await h.snapshot(sample(0,{voltage:0,current:0}));await h.click('tab-power');
  assert.equal(chart(h,'power-voltage-plot').latest,0);
  assert.equal(chart(h,'power-current-plot').latest,0);
  assert.equal(chart(h,'power-watts-plot').latest,0);
  h.advance(1000);await h.snapshot(sample(1,{voltage:6,current:3}));
  assert.equal(chart(h,'power-voltage-plot').latest,6);
  assert.equal(chart(h,'power-current-plot').latest,3);
  assert.equal(chart(h,'power-watts-plot').latest,18);
  const oldCount=chart(h,'power-watts-plot').samples;
  h.advance(1000);await h.snapshot(sample(2,{voltage:6,current:null}));
  assert.equal(chart(h,'power-watts-plot').samples,oldCount,'Missing current cannot produce a false zero-watt sample');
  assert.equal(chart(h,'power-current-plot').samples,oldCount);
  assert.match(h.text('power-watts-plot-current'),/--/);
  assert.notEqual(h.text('power-watts-plot-state'),'LIVE','A missing current cannot label old power as a fresh reading');
  h.advance(1000);await h.snapshot(sample(3,{voltage:6,current:3}));
  assert.equal(chart(h,'power-watts-plot').segments,2,'Missing current must interrupt the power trace');
});

test('chart sampling follows source changes, not HTTP polls or repeated display refreshes',async()=>{
  const h=harness();await h.snapshot(sample(0));await h.click('tab-power');
  assert.equal(chart(h,'power-watts-plot').samples,1);
  h.advance(1000);await h.snapshot(sample(1,{age:1}));
  await h.tick();await h.tick();
  assert.equal(chart(h,'power-watts-plot').samples,1,'Uptime and source age advancing together describes the same sample');
  h.advance(1000);await h.snapshot(sample(2));
  assert.equal(chart(h,'power-watts-plot').samples,2,'A new sample is retained even when its numeric value is unchanged');
  assert.equal(chart(h,'power-watts-plot').latest,24);
});

test('chart traces leave gaps after stale source data and browser outages',async()=>{
  const h=harness();await h.snapshot(sample(0));await h.click('tab-power');
  h.advance(1000);await h.snapshot(sample(1));
  assert.equal(chart(h,'power-watts-plot').segments,1);
  const stale=sample(2);stale.telemetry.battery.stale=true;
  h.advance(1000);await h.snapshot(stale);
  assert.equal(chart(h,'power-watts-plot').samples,2);
  h.advance(1000);await h.snapshot(sample(3));
  assert.equal(chart(h,'power-watts-plot').segments,2,'A stale interval must not be drawn as a continuous measured trace');
  h.advance(1000);h.replies.push(new Error('connection lost'));await h.timer(250);
  h.advance(1000);await h.snapshot(sample(5));
  assert.equal(chart(h,'power-watts-plot').segments,3,'An API outage must not create a false connecting segment');
  for(const line of chart(h,'power-watts-plot').lines)assert.doesNotMatch(line.getAttribute('points')??'',/NaN|Infinity/);
});

test('plot history is bounded while retaining the latest real sample',async()=>{
  const h=harness();await h.snapshot(sample(0));await h.click('tab-power');
  for(let index=1;index<=190;index++){h.advance(1000);await h.snapshot(sample(index,{voltage:index,current:2}));}
  const result=chart(h,'power-watts-plot');
  assert.equal(result.samples,180);
  assert.equal(result.latest,380);
  assert.ok(result.lines.every(line=>(line.getAttribute('points')??'').split(/\s+/).filter(Boolean).length<=180));
});

test('ground or aircraft restart resets histories instead of joining different runs',async()=>{
  for(const restart of ['ground-session','aircraft-session','ground-clock']) {
    const h=harness();await h.snapshot(sample(0));await h.click('tab-power');
    h.advance(1000);await h.snapshot(sample(1));
    assert.equal(chart(h,'power-watts-plot').samples,2);
    const value=sample(2,{voltage:10,current:3});
    if(restart==='ground-session')value.station.session='ground-b';
    if(restart==='aircraft-session')value.telemetry.session='air-b';
    if(restart==='ground-clock')value.station.uptime_ms=100;
    h.advance(1000);await h.snapshot(value);
    assert.equal(chart(h,'power-watts-plot').samples,1,restart);
    assert.equal(chart(h,'power-watts-plot').latest,30);
  }
});

test('expired samples leave the five-minute plot window rather than extending the time axis',async()=>{
  const h=harness();await h.snapshot(sample(0));await h.click('tab-power');
  assert.equal(chart(h,'power-watts-plot').samples,1);
  h.advance(301000);await h.snapshot(sample(301,{voltage:11,current:2}));
  assert.equal(chart(h,'power-watts-plot').samples,1);
  assert.equal(chart(h,'power-watts-plot').latest,22);
});

test('IMU axis controls hide traces without deleting received samples',async()=>{
  const h=harness(),value=sample(0);
  value.telemetry.imus[0].acceleration_m_s2=[1,2,3];
  await h.snapshot(value);await h.click('tab-sensors');
  h.advance(1000);const next=sample(1);next.telemetry.imus[0].acceleration_m_s2=[4,5,6];await h.snapshot(next);
  const count=chart(h,'imu-accel-plot').samples;
  assert.ok(chart(h,'imu-accel-plot').node.querySelectorAll('[data-axis="y"]').length>0);
  await h.click('axis-y');
  assert.equal(h.elements.get('axis-y').getAttribute('aria-pressed'),'false');
  assert.equal(chart(h,'imu-accel-plot').node.querySelectorAll('[data-axis="y"]').length,0);
  assert.ok(chart(h,'imu-accel-plot').node.querySelectorAll('[data-axis="x"]').length>0);
  assert.equal(chart(h,'imu-accel-plot').samples,count);
  await h.click('axis-y');
  assert.equal(h.elements.get('axis-y').getAttribute('aria-pressed'),'true');
  assert.ok(chart(h,'imu-accel-plot').node.querySelectorAll('[data-axis="y"]').length>0);
});

test('power Y-axis labels preserve enough precision to distinguish nearby voltages and currents',async()=>{
  const h=harness();await h.snapshot(sample(0,{voltage:12.5,current:.34}));await h.click('tab-power');
  for(const [id,expected] of [['power-voltage-plot',12.5],['power-current-plot',.34]]) {
    const labels=chart(h,id).node.querySelectorAll('.y-axis-label').map(node=>node.textContent.trim());
    assert.equal(labels.length,5,`${id}: all five Y-axis levels are labeled`);
    assert.equal(new Set(labels).size,5,`${id}: displayed tick values must not collapse after rounding`);
    const values=labels.map(Number);
    assert.ok(values.every(Number.isFinite),`${id}: tick labels stay numeric`);
    assert.ok(values.every((value,index)=>index===0||values[index-1]>value),`${id}: labels strictly descend with the Y-axis`);
    assert.equal(values[2],expected,`${id}: midpoint retains the actual reported reading`);
  }
  h.advance(1000);await h.snapshot(sample(1,{voltage:0,current:0}));
  assert.equal(chart(h,'power-voltage-plot').latest,0);
  assert.equal(chart(h,'power-current-plot').latest,0);
  assert.equal(chart(h,'power-watts-plot').latest,0);
});

test('fast polling uses the compact endpoint and never overlaps a live request',async()=>{
  const h=harness();await h.timer(0);
  assert.equal(h.calls.length,1);
  assert.equal(h.calls[0].url,'/api/live');
  assert.equal(h.livePending(),1);
  assert.equal(h.scheduled().filter(timer=>timer.delay===250).length,0);
  h.advance(1000);await h.tick();await h.tick();
  assert.equal(h.calls.length,1,'Age timers must not start overlapping HTTP reads');
  await h.snapshot(fixture());
  assert.equal(h.livePending(),0);
  assert.equal(h.scheduled().filter(timer=>timer.delay===250).length,1);
  assert.equal(h.calls.filter(call=>call.url==='/api/telemetry').length,0,'Flight needs only the compact snapshot');
});

test('horizon and compass update on a fast response without waiting for the age timer',async()=>{
  const h=harness();await h.snapshot(sample(0));
  const scene=h.elements.get('horizon-scene'),compass=h.elements.get('compass-rose');
  const value=sample(1);value.telemetry.attitude={age_s:0,stale:false,roll_deg:21,pitch_deg:-4,yaw_deg:91};
  h.advance(250);await h.snapshot(value);
  assert.equal(h.elements.get('horizon-scene'),scene);
  assert.equal(h.elements.get('compass-rose'),compass);
  assert.match(scene.getAttribute('transform'),/rotate\(-21\)/);
  assert.match(scene.getAttribute('transform'),/translate\(0[ ,]+-10\)/);
  assert.match(h.text('compass-heading'),/091/);
});

test('cached live polls and age-only ticks retain cards and avoid creating page nodes',async()=>{
  const h=harness();await h.snapshot(sample(0));
  const metric=h.elements.get('metrics').children[0],record=h.elements.get('records').children[0];
  const metricBody=metric.querySelector('.card-body'),recordTable=record.querySelector('table');
  const created=h.created(),metricReplacements=h.elements.get('metrics').replacements,recordReplacements=h.elements.get('records').replacements;
  for(let index=1;index<=4;index++){
    const value=sample(0,{age:index*.25});value.station.uptime_ms+=index*250;
    h.advance(250);await h.snapshot(value);
  }
  await h.tick();
  assert.equal(h.elements.get('metrics').children[0],metric);
  assert.equal(h.elements.get('records').children[0],record);
  assert.equal(metric.querySelector('.card-body'),metricBody);
  assert.equal(record.querySelector('table'),recordTable);
  assert.equal(h.elements.get('metrics').replacements,metricReplacements);
  assert.equal(h.elements.get('records').replacements,recordReplacements);
  assert.equal(h.created(),created,'Cached data must not rebuild cards, tables or instruments');
});

test('cached plot polls retain SVG children and IMU selection while new samples draw promptly',async()=>{
  const h=harness();await h.snapshot(sample(0));await h.click('tab-sensors');
  const select=h.elements.get('imu-select'),option=select.options[0],svg=chart(h,'imu-accel-plot').node;
  const grid=svg.firstChild,replacements=svg.replacements;
  const cached=sample(0,{age:.25});cached.station.uptime_ms+=250;
  h.advance(250);await h.snapshot(cached);await h.tick();
  assert.equal(h.elements.get('imu-select'),select);
  assert.equal(select.options[0],option);
  assert.equal(chart(h,'imu-accel-plot').node,svg);
  assert.equal(svg.firstChild,grid);
  assert.equal(svg.replacements,replacements,'An unchanged plot must not recreate grid and trace nodes');
  const fresh=sample(1);fresh.telemetry.imus[0].acceleration_m_s2=[4,5,6];
  h.advance(250);await h.snapshot(fresh);
  assert.deepEqual(chart(h,'imu-accel-plot').latest,[4,5,6]);
  assert.equal(chart(h,'imu-accel-plot').samples,2);
  assert.equal(chart(h,'imu-accel-plot').node,svg);
});

test('raw JSON is prepared only while expanded and detail fetching stops when no longer needed',async()=>{
  const h=harness();await h.snapshot(sample(0));
  const raw=h.elements.get('raw-json'),writes=raw.textWrites;
  h.advance(1000);await h.snapshot(sample(1));await h.tick();
  assert.equal(raw.textWrites,writes,'Collapsed raw JSON should consume no serialization/render work');
  assert.equal(h.calls.filter(call=>call.url==='/api/telemetry').length,0);
  await h.openRaw(true);
  assert.match(h.text('raw-json'),/cell_voltages_v/);
  assert.equal(h.calls.filter(call=>call.url==='/api/telemetry').length,1);
  await h.openRaw(false);
  const closedWrites=raw.textWrites,fullCount=h.calls.filter(call=>call.url==='/api/telemetry').length;
  h.advance(1000);await h.snapshot(sample(2));await h.tick();
  assert.equal(raw.textWrites,closedWrites);
  assert.equal(h.calls.filter(call=>call.url==='/api/telemetry').length,fullCount);
  assert.equal(h.scheduled().filter(timer=>timer.delay===3000).length,0);
});

test('a delayed full response cannot overwrite newer live flight readings or renew their ages',async()=>{
  const h=harness();await h.snapshot(sample(0));h.deferDetails();await h.click('tab-health');
  assert.equal(h.detailPending(),1);
  const live=sample(1,{voltage:13,current:4});
  live.telemetry.mode='ALT_HOLD';live.telemetry.attitude={age_s:0,stale:false,roll_deg:42,pitch_deg:3,yaw_deg:271};
  h.advance(1000);await h.snapshot(live);
  h.advance(500);await h.full(sample(0));await h.click('tab-overview');
  assert.equal(h.text('flight-mode'),'ALT_HOLD');
  assert.match(h.text('horizon-roll'),/42\.0/);
  assert.match(h.text('compass-heading'),/271/);
  assert.match(h.text('metrics'),/13\.00 V/);
  assert.match(h.text('horizon-state'),/0\.5 s/,'Older full data cannot reset the live record receipt anchor');
});

test('live refreshes preserve the source ages of diagnostics omitted from compact snapshots',async()=>{
  const h=harness(),initial=sample(0);initial.telemetry.mcu.age_s=2;
  await h.snapshot(initial);await h.click('tab-health');
  const mcu=()=>h.elements.get('records').children.find(node=>node.querySelector('h3')?.textContent==='MCU');
  assert.match(mcu().querySelector('.age').textContent,/2\.0 s/);
  for(let index=1;index<=3;index++){h.advance(1000);await h.snapshot(sample(index));}
  assert.match(mcu().querySelector('.age').textContent,/5\.0 s/,'Fast core packets do not refresh old optional sensor data');
});

test('a missing core reading in a live response clears the instrument despite cached full data',async()=>{
  const h=harness();await h.snapshot(sample(0));h.deferDetails();await h.click('tab-health');
  const missing=sample(1);missing.telemetry.attitude=null;
  h.advance(1000);await h.snapshot(missing);await h.full(sample(0));await h.click('tab-overview');
  assert.equal(h.elements.get('instrument-horizon').getAttribute('data-state'),'waiting');
  assert.equal(h.elements.get('instrument-compass').getAttribute('data-heading'),'');
  assert.match(h.text('compass-heading'),/---/);
});

test('detail requests stay bounded and do not block fast live updates',async()=>{
  const h=harness();await h.snapshot(sample(0));await h.click('tab-pipeline');
  assert.equal(h.calls.filter(call=>call.url==='/api/telemetry').length,1);
  h.deferDetails();await h.timer(3000);
  assert.equal(h.detailPending(),1);
  assert.equal(h.scheduled().filter(timer=>timer.delay===3000).length,0);
  const live=sample(1);live.telemetry.mode='LOITER';h.advance(250);await h.snapshot(live);
  assert.equal(h.text('flight-mode'),'LOITER');
  assert.equal(h.detailPending(),1);
  await h.click('tab-health');
  assert.equal(h.calls.filter(call=>call.url==='/api/telemetry').length,2,'Switching tabs must not start another full request while one is pending');
  await h.click('tab-overview');
  assert.equal(h.detailPending(),0);
  assert.equal(h.scheduled().filter(timer=>timer.delay===3000).length,0);
});

test('failure of optional full diagnostics does not mark the fast live connection lost',async()=>{
  const h=harness();await h.snapshot(sample(0));h.deferDetails();await h.click('tab-health');
  await h.full(new Error('Detailed snapshot unavailable'));
  assert.equal(h.text('browser-state'),'Connected');
  h.advance(250);const live=sample(1);live.telemetry.mode='ALT_HOLD';await h.snapshot(live);
  assert.equal(h.text('browser-state'),'Connected');
  assert.equal(h.text('flight-mode'),'ALT_HOLD');
});

test('a delayed full snapshot from an old aircraft session cannot populate current diagnostics',async()=>{
  const h=harness();await h.snapshot(sample(0));h.deferDetails();await h.click('tab-health');
  const restarted=sample(1);restarted.telemetry.session='air-b';
  h.advance(1000);await h.snapshot(restarted);await h.full(sample(0));
  const mcu=h.elements.get('records').children.find(node=>node.querySelector('h3')?.textContent==='MCU');
  assert.ok(mcu,'MCU availability remains inspectable after a restart');
  assert.match(mcu.textContent,/Waiting \/ unavailable/);
});

test('a delayed full response retains server sample age instead of making old diagnostics fresh',async()=>{
  const h=harness(),captured=sample(0);captured.telemetry.mcu.age_s=2;
  await h.snapshot(captured);h.deferDetails();await h.click('tab-health');
  for(let index=1;index<=3;index++){h.advance(1000);await h.snapshot(sample(index));}
  await h.full(captured);
  const mcu=h.elements.get('records').children.find(node=>node.querySelector('h3')?.textContent==='MCU');
  assert.match(mcu.querySelector('.age').textContent,/5\.0 s/,'Three seconds of held HTTP data must advance the original two-second reading age');
  await h.openRaw(true);
  const raw=JSON.parse(h.text('raw-json'));
  assert.equal(raw.telemetry.mcu.age_s,5,'The expanded JSON must report the same effective source age as its card');
});
