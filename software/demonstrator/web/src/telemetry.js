// SPDX-License-Identifier: MIT
// PC performance.now() and Pi monotonic clocks have independent origins.
export class ClockSync {
  constructor(){this.samples=[];this.best=null;}
  add(t0,t1,t2,t3){
    if(![t0,t1,t2,t3].every(Number.isFinite)||t3<t0||t2<t1)return false;
    const rtt=(t3-t0)-(t2-t1);
    if(rtt<0||rtt>2000)return false;
    this.samples=this.samples.filter(s=>t3-s.at<30000);
    this.samples.push({offset:((t1-t0)+(t2-t3))/2,rtt,at:t3});
    if(this.samples.length>20)this.samples.shift();
    this.best=this.samples.reduce((a,b)=>a.rtt<b.rtt?a:b);
    return true;
  }
  age(captured,now){
    if(!this.best||now-this.best.at>30000||!Number.isFinite(captured))return null;
    const value=now+this.best.offset-captured;
    if(value < -this.best.rtt/2-2 || value>60000)return null;
    return Math.max(0,value);
  }
  get uncertainty(){return this.best?this.best.rtt/2:null;}
}
export class FrameWindow {
  constructor(){this.reset();}
  reset(){this.samples=[];this.previous=null;this.skipped=0;}
  add(sequence,bytes,now){
    if(!Number.isFinite(sequence)||!Number.isFinite(bytes)||bytes<0)return;
    if(this.previous!==null&&sequence>this.previous)this.skipped+=Math.max(0,sequence-this.previous-1);
    this.previous=sequence;
    this.samples.push({sequence,bytes,at:now});
    this.samples=this.samples.filter(s=>now-s.at<2500);
  }
  stats(now){
    this.samples=this.samples.filter(s=>now-s.at<2500);
    if(this.samples.length<2)return {fps:0,mbps:0};
    const elapsed=(this.samples.at(-1).at-this.samples[0].at)/1000;
    return elapsed>0?{fps:(this.samples.length-1)/elapsed,mbps:this.samples.slice(1).reduce((n,s)=>n+s.bytes,0)*8/elapsed/1e6}:{fps:0,mbps:0};
  }
}
