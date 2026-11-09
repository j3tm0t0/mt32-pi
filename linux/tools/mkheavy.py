#!/usr/bin/env python3
# Dense multi-channel SMF to push FluidSynth polyphony (approx. 200+ simultaneous voices)
import struct, sys, random
random.seed(1)
def vlq(n):
    b=[n&0x7f]; n>>=7
    while n: b.insert(0,(n&0x7f)|0x80); n>>=7
    return bytes(b)
evs=[]  # (tick, bytes)
evs.append((0,[0xF0,5,0x7E,0x7F,0x09,0x01,0xF7]))
progs=[0,4,24,32,40,48,52,56,61,64,73,80,88,89,90]
for ch in range(16):
    if ch!=9: evs.append((0,[0xC0|ch,progs[ch%len(progs)]]))
    evs.append((0,[0xB0|ch,91,100])); evs.append((0,[0xB0|ch,93,80]))
tick=0
for step in range(int(sys.argv[2]) if len(sys.argv)>2 else 600):   # 50ms steps at 120bpm/480ppq => 48 ticks
    for ch in range(16):
        n=random.randint(36,84) if ch!=9 else random.choice([35,36,38,42,46,49,51])
        evs.append((tick,[0x90|ch,n,random.randint(60,120)]))
        evs.append((tick+480*2,[0x80|ch,n,0]))  # hold 1s
    tick+=48
evs.sort(key=lambda e:e[0])
out=b''; last=0
for t,b in evs: out+=vlq(t-last)+bytes(b); last=t
out+=vlq(0)+bytes([0xFF,0x2F,0])
open(sys.argv[1],'wb').write(b'MThd'+struct.pack('>IHHH',6,0,1,480)+b'MTrk'+struct.pack('>I',len(out))+out)
