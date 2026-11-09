import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {RoomEnvironment} from 'three/addons/environments/RoomEnvironment.js';
import {MeshoptDecoder} from 'three/addons/libs/meshopt_decoder.module.js';

export function createRobotView(container,onSelect){
  let renderer;
  try{renderer=new THREE.WebGLRenderer({antialias:true,alpha:true,powerPreference:'high-performance'});}catch(error){document.querySelector('#model-loading').textContent='3D is unavailable in this browser. All hardware controls remain available.';return {update(){},view(){}};}
  renderer.setPixelRatio(Math.min(devicePixelRatio,2));
  renderer.setClearColor(0xedf0ee,0);
  renderer.outputColorSpace=THREE.SRGBColorSpace;
  renderer.toneMapping=THREE.ACESFilmicToneMapping;
  renderer.toneMappingExposure=1.05;
  renderer.shadowMap.enabled=true;
  renderer.shadowMap.type=THREE.PCFSoftShadowMap;
  container.appendChild(renderer.domElement);
  const scene=new THREE.Scene();
  const camera=new THREE.PerspectiveCamera(35,1,.001,10);
  const controls=new OrbitControls(camera,renderer.domElement);
  controls.target.set(0,.042,.018);controls.enableDamping=true;controls.dampingFactor=.09;
  controls.minDistance=.17;controls.maxDistance=.7;controls.maxPolarAngle=Math.PI-.03;
  const pmrem=new THREE.PMREMGenerator(renderer);
  const room=new RoomEnvironment();scene.environment=pmrem.fromScene(room,.04).texture;room.dispose();pmrem.dispose();
  scene.environmentIntensity=.7;
  scene.add(new THREE.HemisphereLight(0xf5fbff,0x78847b,1.6));
  const key=new THREE.DirectionalLight(0xfff7ec,2.7);key.position.set(-.15,.45,-.3);key.castShadow=true;
  key.shadow.mapSize.set(2048,2048);key.shadow.radius=4;key.shadow.camera.left=-.18;key.shadow.camera.right=.18;key.shadow.camera.top=.18;key.shadow.camera.bottom=-.18;key.shadow.camera.near=.01;key.shadow.camera.far=1;key.shadow.bias=-.00008;key.shadow.normalBias=.0004;scene.add(key);
  const fill=new THREE.DirectionalLight(0xddeeff,1.1);fill.position.set(.3,.22,.18);scene.add(fill);
  const floor=new THREE.Mesh(new THREE.PlaneGeometry(2,2),new THREE.ShadowMaterial({opacity:.08}));floor.rotation.x=-Math.PI/2;floor.position.y=-.0001;floor.receiveShadow=true;scene.add(floor);
  const robotRoot=new THREE.Group();scene.add(robotRoot);
  const raycaster=new THREE.Raycaster(),pointer=new THREE.Vector2();let robot=null,state=null,selection=null;
  const emitters=Array.from({length:6},()=>[]),buttons=Array.from({length:4},()=>[]);const wheels={left:null,right:null};
  const lampPositions=[[.0175,.0269,-.05875],[-.0175,.0269,-.05875],[-.033,.0269,-.011],[-.028,.0269,.04925],[.028,.0269,.04925],[.033,.0269,-.011]];
  const lamps=lampPositions.map(pos=>{const l=new THREE.PointLight(0xffffff,0,.055,2);l.position.set(...pos);scene.add(l);return l;});
  function typeFor(obj){let current=obj;while(current){const n=current.name;let match;
    if((match=n.match(/^RGB([1-6])(?:_|$)/)))return {type:'leds',index:Number(match[1])-1,label:`RGB ${match[1]} · underside light`};
    if((match=n.match(/^(?:SW|ButtonLED)([1-4])(?:_|$)/)))return {type:'button',index:Number(match[1])-1,label:`Button ${['A','B','X','Y'][Number(match[1])-1]}`};
    if(/Wheel|Gearmotor/.test(n))return {type:'drive',label:/Left|left/.test(n)?'Left drive wheel':'Right drive wheel'};
    if(/HC.?SR04|Ultrasonic|RangeSensor/i.test(n))return {type:'sensors',label:'HC-SR04 · ultrasonic ranging'};
    if(/Camera/.test(n))return {type:'camera',label:'Raspberry Pi camera'};
    current=current.parent;}return null;}
  new GLTFLoader().setMeshoptDecoder(MeshoptDecoder).load('./assets/robot.glb',gltf=>{
    robot=gltf.scene;robotRoot.add(robot);
    robot.traverse(obj=>{
      if(obj.name==='WheelLeft')wheels.left=obj;if(obj.name==='WheelRight')wheels.right=obj;
      if(!obj.isMesh)return;obj.castShadow=true;obj.receiveShadow=true;
      const m=obj.name.match(/^RGB([1-6])_Emitter/);const b=obj.name.match(/^ButtonLED([1-4])_Emitter/);
      if(m||b){obj.material=obj.material.clone();(m?emitters[Number(m[1])-1]:buttons[Number(b[1])-1]).push(obj);}
    });
    document.querySelector('#model-loading').hidden=true;
    if(state)update(state);
  },undefined,error=>{document.querySelector('#model-loading').textContent='The model could not load. Hardware controls still work.';console.error(error);});
  function view(name='hero'){
    const positions={hero:[.17,.145,-.195],top:[.001,.35,.018],bottom:[.14,-.20,-.14]};
    camera.position.set(...(positions[name]||positions.hero));controls.target.set(0,.042,.018);floor.visible=name!=='bottom';controls.update();
    document.querySelectorAll('[data-view]').forEach(b=>b.classList.toggle('active',b.dataset.view===name));
  }view();
  new ResizeObserver(()=>{const w=container.clientWidth,h=container.clientHeight;if(w&&h){renderer.setSize(w,h);camera.aspect=w/h;camera.updateProjectionMatrix();}}).observe(container);
  const tooltip=document.querySelector('#model-tooltip');let down=null;
  function pick(event){if(!robot)return null;const r=renderer.domElement.getBoundingClientRect();pointer.set((event.clientX-r.left)/r.width*2-1,-(event.clientY-r.top)/r.height*2+1);raycaster.setFromCamera(pointer,camera);for(const hit of raycaster.intersectObject(robot,true)){const t=typeFor(hit.object);if(t)return t;return null;}return null;}
  renderer.domElement.addEventListener('pointerdown',e=>down=[e.clientX,e.clientY]);
  renderer.domElement.addEventListener('pointerup',e=>{if(down&&Math.hypot(e.clientX-down[0],e.clientY-down[1])<5){const found=pick(e);if(found)onSelect(found);}down=null;});
  renderer.domElement.addEventListener('pointermove',e=>{const found=pick(e);tooltip.hidden=!found;if(found){const r=container.getBoundingClientRect();tooltip.textContent=found.label;tooltip.style.left=`${Math.min(e.clientX-r.left+12,r.width-190)}px`;tooltip.style.top=`${e.clientY-r.top-28}px`;}renderer.domElement.style.cursor=found?'pointer':'grab';});
  renderer.domElement.addEventListener('pointerleave',()=>tooltip.hidden=true);
  function update(next){state=next;const r=next.robot||{};for(let i=0;i<6;i++){const color=r.rgb?.[i]||0;for(const mesh of emitters[i]){mesh.material.emissive.setHex(color);mesh.material.emissiveIntensity=color?2.1:0;mesh.material.color.setHex(color||0xc4c6c3);}lamps[i].color.setHex(color);lamps[i].intensity=color?.004:0;}for(let i=0;i<4;i++){for(const mesh of buttons[i]){mesh.material.emissive.setHex(0xfff8df);mesh.material.emissiveIntensity=(r.buttonLeds&(1<<i))?2:0;}}}
  let last=performance.now();const reduce=matchMedia('(prefers-reduced-motion: reduce)');
  function draw(now){const dt=Math.min(.05,(now-last)/1000);last=now;if(!document.hidden){if(state?.robot?.motorEnabled&&!reduce.matches){if(wheels.left)wheels.left.rotation.x+=state.robot.left/1000*8*dt;if(wheels.right)wheels.right.rotation.x+=state.robot.right/1000*8*dt;}controls.update();renderer.render(scene,camera);}requestAnimationFrame(draw);}requestAnimationFrame(draw);
  return {update,view};
}
