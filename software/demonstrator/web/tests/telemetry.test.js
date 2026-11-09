import {test} from 'node:test';
import assert from 'node:assert/strict';
import {ClockSync,FrameWindow} from '../src/telemetry.js';
test('clock offset cancels unrelated PC and Pi origins',()=>{const c=new ClockSync();c.add(100,1000102,1000103,105);assert.equal(c.best.offset,1000000);assert.equal(c.best.rtt,4);assert.equal(c.age(1000200,225),25);assert.equal(c.uncertainty,2);});
test('lowest network RTT rejects variable network queuing bias',()=>{const c=new ClockSync();c.add(100,1002,1003,105);c.add(200,1150,1151,280);assert.equal(c.best.offset,900);assert.equal(c.best.rtt,4);});
test('stale, reversed and impossible samples do not fabricate latency',()=>{const c=new ClockSync();assert.equal(c.age(10,20),null);assert.equal(c.add(1,3,2,4),false);c.add(100,1002,1003,105);assert.equal(c.age(40000,40000),null);assert.equal(c.age(2000,110),null);});
test('frame throughput counts arrivals and explicit sequence gaps',()=>{const w=new FrameWindow();w.add(10,1000,0);w.add(11,2000,100);w.add(14,2000,200);assert.equal(w.skipped,2);assert.deepEqual(w.stats(200),{fps:10,mbps:.16});assert.deepEqual(w.stats(4000),{fps:0,mbps:0});});
