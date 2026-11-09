import './style.css';
import {ClockSync,FrameWindow} from './telemetry.js';
import {createRobotView} from './model.js';

const $=s=>document.querySelector(s),$$=s=>[...document.querySelectorAll(s)];
// randomUUID is unavailable on plain HTTP LAN origins in some browsers.
const random=crypto.getRandomValues(new Uint32Array(4));
const clientId=Array.from(random,x=>x.toString(16).padStart(8,'0')).join('-');
let token='',seq=0,state=null,connected=false,owner=false,selectedLed=-1,driveDirection=null,driveBusy=false;
let streamEpoch=0,streamRunning=false,frameAbort=null,frameSequence=0,photoBlob=null,toastTimeout=null,pollBusy=false;
let photoBusy=false,cameraBusy=false,stopPending=0,lastDrawAt=0,lastCaptured=NaN;
let sync=new ClockSync(),frames=new FrameWindow(),delays=[];
const robot=createRobotView($('#model-stage'),selected=>{
  if(selected.type==='camera'){$('.camera-card').scrollIntoView({behavior:matchMedia('(prefers-reduced-motion: reduce)').matches?'instant':'smooth',block:'center'});return;}
  if(selected.type==='button'){tab('leds');toast('Use the button indicators below to toggle its light.');return;}
  tab(selected.type);if(selected.type==='leds')selectLed(selected.index);
});

function log(message){const li=document.createElement('li');li.textContent=`${new Date().toLocaleTimeString([], {hour12:false})}  ${message}`;$('#activity-log').prepend(li);while($('#activity-log').children.length>15)$('#activity-log').lastChild.remove();}
function toast(message,error=false){$('#toast').textContent=message;$('#toast').classList.toggle('error',error);$('#toast').hidden=false;clearTimeout(toastTimeout);toastTimeout=setTimeout(()=>$('#toast').hidden=true,4500);}
function number(selector,value,unit='',digits=0){const n=Number.isFinite(value)?value.toFixed(digits):'—';$(selector).replaceChildren(document.createTextNode(n));if(unit){const small=document.createElement('small');small.textContent=unit;$(selector).append(small);}}
function tab(name){$$('[data-tab]').forEach(b=>b.setAttribute('aria-selected',String(b.dataset.tab===name)));$$('.tab-panel').forEach(p=>p.hidden=p.id!==`panel-${name}`);}
$$('[data-tab]').forEach(b=>b.onclick=()=>tab(b.dataset.tab));
$$('[data-view]').forEach(b=>b.onclick=()=>robot.view(b.dataset.view));$('#reset-view').onclick=()=>robot.view();$('#led-view').onclick=()=>robot.view('bottom');

async function api(path,options={}){
  const headers={Authorization:`Bearer ${token}`,...options.headers};
  const response=await fetch(path,{...options,headers,cache:'no-store',signal:options.signal||AbortSignal.timeout(12000)});
  if(!response.ok){let message=`Request failed (${response.status})`;try{message=(await response.json()).error||message;}catch{}const e=new Error(message);e.status=response.status;throw e;}return response;
}
async function command(type,fields={},quiet=false){
  if(!connected)throw new Error('Connect to the robot first.');
  const safetyStop=['stop','disarm','estop','release'].includes(type);if(safetyStop)stopPending++;
  try{const response=await api('/api/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({type,clientId,seq:++seq,...fields})});const data=await response.json();if(!quiet&&!['heartbeat','drive'].includes(type))log(({claim:'Controller acquired',release:'Controller released',arm:'Arming requested',disarm:'Motors disarmed',stop:'Motion stopped',estop:'Emergency stop requested',clearFault:'Fault clear requested',rgb:'RGB lighting updated',buttonLeds:'Button indicators updated',range:'Range measurement requested',cameraStart:'Camera starting',cameraStop:'Camera stopped',photo:'Photo captured',autofocus:'Autofocus requested'})[type]||type);return data;}catch(error){if(!quiet){toast(error.message,true);log(error.message);}throw error;}finally{if(safetyStop)stopPending--;}
}
function act(type,fields){return command(type,fields).catch(()=>null);}

$('#connect-open').onclick=()=>{$('#connect-error').textContent='';$('#connect-dialog').showModal();$('#token').focus();};
$('#connect-cancel').onclick=()=>$('#connect-dialog').close();
$('#connect-form').onsubmit=async event=>{event.preventDefault();token=$('#token').value.trim();try{state=await(await api('/api/state')).json();connected=true;sync=new ClockSync();renderState(state);$('#connect-dialog').close();$('#token').value='';log(`Connected to ${state.language} · ${state.mode}`);await synchronize();}catch(error){token='';$('#connect-error').textContent=error.message;}};
$('#claim').onclick=async()=>{if(!connected){$('#connect-open').click();return;}if(owner){await act('release');owner=false;}else{const ok=await act('claim');if(ok)owner=true;}await poll();};

async function synchronize(){if(!connected)return;try{for(let i=0;i<4;i++){const t0=performance.now(),r=await api('/api/ping'),p=await r.json(),t3=performance.now();sync.add(t0,p.serverReceiveMs,p.serverSendMs,t3);}number('#rtt',sync.best?.rtt,'ms',1);$('#sync-uncertainty').textContent=sync.uncertainty===null?'—':`± ${sync.uncertainty.toFixed(1)} ms`;}catch{}}
setInterval(synchronize,5000);
async function poll(){if(!connected||pollBusy)return;pollBusy=true;try{const next=await(await api('/api/state',{signal:AbortSignal.timeout(2500)})).json();renderState(next);}catch(error){connected=false;owner=false;driveDirection=null;stopFrames();$('#connection-dot').classList.remove('online');$('#connection-label').textContent='Connection lost';$('#robot-state').textContent='OFFLINE';log(`Connection lost: ${error.message}. Server deadlines stop motion.`);toast('Connection lost. Reconnect before controlling the robot.',true);renderEnabled();}finally{pollBusy=false;}}
setInterval(poll,250);
const stale=error=>error.status===409&&/stale command sequence/i.test(error.message);
setInterval(()=>{if(connected&&owner&&!document.hidden&&!stopPending)command('heartbeat',{},true).catch(error=>{if(stale(error))return;owner=false;driveDirection=null;renderEnabled();});},300);

function renderState(next){state=next;owner=next.controller?.clientId===clientId;const r=next.robot||{},c=next.camera||{},loop=next.loop||{};
  $('#connection-dot').classList.add('online');$('#connection-label').textContent='Connected';$('#connect-open').textContent='Connection';$('#backend-badge').textContent=`${next.language} / ${next.mode}`;
  $('#mode-banner').hidden=next.mode==='hardware';$('#mode-banner').textContent=next.mode==='mock'?'MOCK MODE — controls and camera are synthetic. No physical hardware is connected.':'SIMULATOR — board behaviour is simulated; check the camera source separately.';
  $('#robot-state').textContent=r.state||'UNAVAILABLE';$('#robot-state').classList.toggle('armed',r.state==='ARMED');$('#robot-state').classList.toggle('faulted',r.state==='FAULTED');
  $('#motor-permission').textContent=r.motorEnabled?'Motor gate enabled':r.state==='ARMING'?'Arming…':'Motors disarmed';
  number('#left-duty',(r.left||0)/10,'%',1);number('#right-duty',(r.right||0)/10,'%',1);
  $('#claim').textContent=owner?'Release control':next.controller?.clientId?'Controller busy':'Take control';
  $('#distance').textContent=Number.isFinite(r.distanceMm)?Math.round(r.distanceMm):'—';
  $$('[data-button-led]').forEach(b=>b.classList.toggle('on',Boolean(r.buttonLeds&Number(b.dataset.buttonLed))));
  $$('[data-physical]').forEach(b=>b.classList.toggle('on',Boolean(r.buttons&Number(b.dataset.physical))));
  $('#fault-box').hidden=r.state!=='FAULTED';$('#fault-message').textContent=typeof r.fault==='string'?r.fault:r.fault?JSON.stringify(r.fault):'Inspect the cause before clearing. Motors stay disarmed after clearing.';
  number('#loop-gap',loop.maxGapMs,'ms',1);$('#loop-note').textContent=loop.running?`${Number(loop.cycles||0).toLocaleString()} cycles · ${loop.overruns||0} overruns`:'Control loop not running';
  $('#camera-drops').textContent=Number(c.dropped||0).toLocaleString();$('#cpu-temperature').textContent=Number.isFinite(next.system?.cpuTempC)?`${next.system.cpuTempC.toFixed(1)} °C`:'Not available';
  const sec=Math.floor(next.system?.uptimeS||0);$('#server-uptime').textContent=`${Math.floor(sec/3600)}h ${Math.floor(sec/60)%60}m ${sec%60}s`;
  $('#camera-format').textContent=`${c.width||640} × ${c.height||480} · MJPEG · ${c.source||'camera'}`;
  $('#autofocus').textContent=c.autofocus?'Autofocus':'Fixed-focus / unavailable';
  const active=c.state==='streaming';$('#camera-toggle').textContent=active?'Stop stream':'Start stream';$('#stream-indicator').hidden=!active;
  if(!cameraBusy){if(active&&!streamRunning)startFrames();else if(!active&&streamRunning)stopFrames();}
  if(c.error){$('#camera-placeholder').hidden=false;$('#camera-placeholder strong').textContent='Camera unavailable';$('#camera-placeholder span').textContent=c.error;}
  robot.update(next);renderEnabled();
}
function renderEnabled(){const r=state?.robot||{},can=connected&&owner;
  $('#arm').disabled=!can||!state?.motionAllowed||r.state!=='DISARMED';$('#disarm').disabled=!connected;$('#estop').disabled=!connected;
  $$('[data-drive]').forEach(b=>b.disabled=!can||r.state!=='ARMED'||!state?.motionAllowed);
  ['apply-color','led-off','measure','clear-fault','photo','camera-toggle'].forEach(id=>$('#'+id).disabled=!can);
  $('#photo').disabled=!can||photoBusy||cameraBusy;$('#camera-toggle').disabled=!can||cameraBusy||photoBusy;
  $$('[data-color],[data-button-led]').forEach(b=>b.disabled=!can);$('#autofocus').disabled=!can||!state?.camera?.autofocus;
  if(!can||r.state!=='ARMED')releaseDrive(false);
}
$('#arm').onclick=()=>act('arm');$('#disarm').onclick=()=>{releaseDrive(false);act('disarm');};$('#estop').onclick=()=>{releaseDrive(false);act('estop');};$('#clear-fault').onclick=()=>act('clearFault');
$('#speed').oninput=()=>$('#speed-label').textContent=`${Number($('#speed').value)/10}%`;
function releaseDrive(send=true){const had=Boolean(driveDirection);driveDirection=null;$$('[data-drive]').forEach(b=>b.classList.remove('active'));if(had&&send&&connected&&owner)command('drive',{left:0,right:0},true).catch(()=>act('stop'));}
function beginDrive(direction){if(!connected||!owner||state?.robot?.state!=='ARMED'||!state.motionAllowed)return;driveDirection=direction;$$('[data-drive]').forEach(b=>b.classList.toggle('active',b.dataset.drive===direction));sendDrive();}
async function sendDrive(){if(!driveDirection||driveBusy||stopPending)return;driveBusy=true;const p=Number($('#speed').value),values={forward:[p,p],backward:[-p,-p],left:[-p,p],right:[p,-p]}[driveDirection];try{await command('drive',{left:values[0],right:values[1]},true);}catch(error){if(!stale(error)){releaseDrive(false);toast(error.message,true);}}finally{driveBusy=false;}}
setInterval(sendDrive,100);
$$('[data-drive]').forEach(b=>{b.onpointerdown=e=>{e.preventDefault();b.setPointerCapture(e.pointerId);beginDrive(b.dataset.drive);};b.onpointerup=()=>releaseDrive();b.onpointercancel=()=>{releaseDrive(false);act('stop');};b.onlostpointercapture=()=>releaseDrive();});
const keyDirections={w:'forward',a:'left',s:'backward',d:'right'};
document.addEventListener('keydown',e=>{if(['INPUT','TEXTAREA','SELECT'].includes(document.activeElement?.tagName)||$('#connect-dialog').open)return;if(e.code==='Space'){e.preventDefault();if(!e.repeat)$('#estop').click();}else if(e.key==='Escape'){$('#disarm').click();}else if(keyDirections[e.key.toLowerCase()]){e.preventDefault();if(!e.repeat)beginDrive(keyDirections[e.key.toLowerCase()]);}});
document.addEventListener('keyup',e=>{if(keyDirections[e.key.toLowerCase()])releaseDrive();});
function relinquish(){releaseDrive(false);if(connected&&owner){owner=false;command('release',{},true).catch(()=>{});renderEnabled();}}
window.addEventListener('blur',relinquish);document.addEventListener('visibilitychange',()=>{if(document.hidden)relinquish();});
window.addEventListener('pagehide',()=>{if(token&&owner)fetch('/api/command',{method:'POST',keepalive:true,headers:{Authorization:`Bearer ${token}`,'Content-Type':'application/json'},body:JSON.stringify({type:'release',clientId,seq:++seq})}).catch(()=>{});});

function selectLed(index){selectedLed=index;$$('[data-led]').forEach(b=>b.classList.toggle('active',Number(b.dataset.led)===index));$('#model-detail').textContent=index<0?'All six underside RGB lights':`RGB ${index+1} selected`;}
$$('[data-led]').forEach(b=>b.onclick=()=>selectLed(Number(b.dataset.led)));
$('#led-color').oninput=()=>$('#hex-color').textContent=$('#led-color').value.toUpperCase();
const applyColor=color=>act('rgb',{index:selectedLed,color:parseInt(color.replace('#',''),16)});
$('#apply-color').onclick=()=>applyColor($('#led-color').value);$('#led-off').onclick=()=>act('rgb',{index:selectedLed,color:0});
$$('[data-color]').forEach(b=>b.onclick=()=>{$('#led-color').value=b.dataset.color;$('#led-color').oninput();applyColor(b.dataset.color);});
$$('[data-button-led]').forEach(b=>b.onclick=()=>act('buttonLeds',{mask:(state?.robot?.buttonLeds||0)^Number(b.dataset.buttonLed)}));
$('#measure').onclick=()=>act('range');

$('#camera-toggle').onclick=async()=>{if(cameraBusy||photoBusy)return;cameraBusy=true;renderEnabled();const running=state?.camera?.state==='streaming';if(running)stopFrames();else{frameSequence=0;frames.reset();delays=[];}try{await act(running?'cameraStop':'cameraStart');}finally{cameraBusy=false;await poll();renderEnabled();}};
$('#autofocus').onclick=()=>act('autofocus');
$('#photo').onclick=async()=>{if(photoBusy||cameraBusy)return;photoBusy=true;const b=$('#photo');renderEnabled();b.textContent='Capturing…';try{const data=await command('photo');if(data.photo){photoBlob=await(await api(data.photo)).blob();$('#photo-result').hidden=false;toast('Photo ready to download.');}}catch{}finally{photoBusy=false;b.textContent='Take photo';renderEnabled();}};
$('#download-photo').onclick=()=>{if(!photoBlob)return;const url=URL.createObjectURL(photoBlob),a=document.createElement('a');a.href=url;a.download=`keel-${new Date().toISOString().replace(/[:.]/g,'-')}.jpg`;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);};
function stopFrames(){streamRunning=false;streamEpoch++;frameAbort?.abort();frameAbort=null;$('#stream-indicator').hidden=true;$('#camera-placeholder').hidden=false;$('#camera-placeholder strong').textContent='Camera stopped';$('#camera-placeholder span').textContent='Start the stream to see the camera feed.';number('#camera-fps',null,'fps');number('#camera-delay',null,'ms');}
async function startFrames(){streamRunning=true;const epoch=++streamEpoch;frameSequence=0;lastDrawAt=performance.now();lastCaptured=NaN;
  while(streamRunning&&connected&&epoch===streamEpoch){
    if(document.hidden){await new Promise(r=>setTimeout(r,250));continue;}
    frameAbort=new AbortController();const timeout=setTimeout(()=>frameAbort?.abort(),5000);
    try{
      const response=await api(`/api/camera/frame?after=${frameSequence}`,{signal:frameAbort.signal});
      clearTimeout(timeout);if(response.status===204){await new Promise(r=>setTimeout(r,30));continue;}
      const blob=await response.blob();const bitmap=await createImageBitmap(blob);
      if(epoch!==streamEpoch||!streamRunning){bitmap.close();break;}
      const sequence=Number(response.headers.get('X-Sequence'));
      const capturedHeader=response.headers.get('X-Captured-Ms');const captured=capturedHeader===null?NaN:Number(capturedHeader);
      const basis=response.headers.get('X-Timestamp-Basis')||'server-receipt';
      await new Promise(resolve=>requestAnimationFrame(()=>{const canvas=$('#camera-canvas');if(canvas.width!==bitmap.width||canvas.height!==bitmap.height){canvas.width=bitmap.width;canvas.height=bitmap.height;}canvas.getContext('2d').drawImage(bitmap,0,0);bitmap.close();resolve();}));
      if(epoch!==streamEpoch)break;
      const now=performance.now();lastDrawAt=now;lastCaptured=captured;frameSequence=sequence;frames.add(sequence,blob.size,now);const stats=frames.stats(now);const age=sync.age(captured,now);
      $('#stream-indicator').textContent='● LIVE';$('#stream-indicator').style.color='';
      $('#camera-placeholder').hidden=true;$('#frame-counter').textContent=`FRAME ${sequence}`;number('#camera-fps',stats.fps,'fps',1);number('#camera-delay',age,'ms',0);$('#throughput').textContent=`${stats.mbps.toFixed(2)} Mbit/s`;$('#client-skips').textContent=frames.skipped.toLocaleString();
      const sensor=basis==='sensor';$('#delay-label').textContent=sensor?'ESTIMATED FRAME AGE':'RECEIVED-FRAME AGE';$('#delay-note').textContent=age===null?'Waiting for clock synchronization':`${sensor?'Sensor timestamp':'Server receipt'} → browser draw`;
      $('#timing-explainer').textContent=sensor?'Frame age uses camera timestamps and an estimate of the Pi’s clock. It includes transfer and decoding up to browser draw, not monitor scanout. Network RTT is measured separately.':'Camera capture timing is unavailable for this source. The value measures server receipt to browser draw and excludes sensor capture and encoding. It is not full camera latency.';
      if(age!==null){delays.push(age);if(delays.length>60)delays.shift();drawChart();}
    }catch(error){clearTimeout(timeout);if(epoch===streamEpoch&&streamRunning){await new Promise(r=>setTimeout(r,200));}}
  }
}
function drawChart(){const maximum=Math.max(100,Math.ceil(Math.max(...delays)/50)*50);const points=delays.map((v,i)=>`${i/(Math.max(2,delays.length)-1)*500},${115-v/maximum*105}`).join(' ');$('#chart-line').setAttribute('points',points);$('#chart-fill').setAttribute('d',`M0,125 L${points.replaceAll(' ', ' L')} L500,125Z`);$('#chart-scale').textContent=`${maximum} ms`;$('#chart-value').textContent=`${Math.round(delays.at(-1))} ms`;}
setInterval(()=>{if(!streamRunning)return;const now=performance.now();if(now-lastDrawAt>1000){number('#camera-fps',0,'fps',1);number('#camera-delay',sync.age(lastCaptured,now),'ms');$('#throughput').textContent='0.00 Mbit/s';$('#delay-note').textContent=`Waiting for a fresh frame · ${((now-lastDrawAt)/1000).toFixed(1)} s`;
$('#stream-indicator').textContent='● FRAME STALE';$('#stream-indicator').style.color='#ffbc50';$('#chart-value').textContent='Waiting for fresh frames';}},250);
renderEnabled();
