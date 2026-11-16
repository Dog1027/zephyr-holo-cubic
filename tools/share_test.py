#!/usr/bin/env python3
"""Test the Share app on the real device: reset, open Share, read the PIN from the log, then
list / upload / download / mkdir / delete with curl and check the PIN rules.

Usage: share_test.py PORT
Needs Wi-Fi (the cube and this computer on the same network) and curl. Takes about a minute,
half of it waiting for the wrong-PIN lock to end. Close any serial monitor first.
Everything it creates is in /st on the SD card and is deleted again.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time

import serial

T = tempfile.mkdtemp(prefix="share_test_")
ser = serial.Serial()
ser.port = sys.argv[1]
ser.baudrate = 115200
ser.timeout = 0.05
# CP2102 DTR/RTS drive the ESP32 boot pins, keep both released when opening
ser.dtr = False
ser.rts = False
ser.open()
log=open(T+"/share_serial.log","w")
def read_for(s):
    end=time.time()+s; b=b""
    while time.time()<end: b+=ser.read(4096)
    t=re.sub(r"\x1b\[[0-9;]*[A-Za-z]","",b.decode(errors="replace")); log.write(t); log.flush(); return t
def cmd(l,w):
    for ch in l+"\r": ser.write(ch.encode()); time.sleep(0.01)
    return read_for(w)
ser.rts=True; time.sleep(0.1); ser.rts=False; boot=read_for(10)
ip=re.search(r"online, IP (\S+)",boot).group(1)
pin=re.search(r"PIN (\d{4})",cmd("app open Share",2.5)).group(1)
B=f"http://{ip}"; fails=[]
def curl(*a, pin_=True, out=None):
    c=["curl","-sS","-m","40","-w","\n%{http_code} %{speed_download} %{speed_upload}"]+(["-b","pin="+pin] if pin_ else [])+(["-o",out] if out else [])+list(a)
    r=subprocess.run(c,capture_output=True,text=True); read_for(0.2)
    body,_,tail=r.stdout.rpartition("\n"); code,sd,su=(tail.split()+["0","0","0"])[:3]
    return int(code or 0), body, float(sd), float(su), r.stderr.strip()
def check(name, cond, info=""):
    print(("ok   " if cond else "FAIL ")+name+(" — "+str(info) if info else ""), flush=True)
    if not cond: fails.append(name)
open(T+"/small.txt","w").write("hello cube\n"); os.system(f"head -c 300000 /dev/urandom > {T}/big.bin")
c,b,*_=curl(B+"/",pin_=False); check("page without PIN", c==200 and "<script>" in b, f"{len(b)} bytes"); 
c,b,*_=curl(B+"/?list",pin_=False); check("list without PIN is 401", c==401, c)
c,b,*_=curl(B+"/config.txt",pin_=False); check("download without PIN is 401", c==401, c)
c,b,*_=curl("-T",T+"/small.txt",B+"/st/",pin_=False); check("upload without PIN is 401", c==401, c)
c,b,*_=curl(B+"/?list"); root=json.loads(b) if c==200 else []; check("list root", c==200 and any(e["n"]=="config.txt" for e in root), f"{len(root)} entries")
c,b,*_=curl(B+"/config.txt",out=T+"/cfg.out"); check("download config.txt with PIN", c==200 and os.path.getsize(T+"/cfg.out")>0, c)
c,b,*_=curl("-T",T+"/small.txt",B+"/st/"); check("upload small", c==201, b.strip())
c,b,_,su,e=curl("-T",T+"/big.bin",B+"/st/big.bin"); check("upload 300 KB", c==201, f"{su/1024:.0f} KB/s {e}")
c,b,*_=curl("-X","PUT",B+"/st/sub/"); check("mkdir", c==201, b.strip())
c,b,*_=curl(B+"/st/?list"); l=json.loads(b) if c==200 else []
check("list folder", {(e["n"],e["d"],e["s"]) for e in l}=={("small.txt",0,11),("big.bin",0,300000),("sub",1,0)}, l)
c,b,sd,_,e=curl(B+"/st/big.bin",out=T+"/big.out")
same=os.path.exists(T+"/big.out") and hashlib.md5(open(T+"/big.out","rb").read()).digest()==hashlib.md5(open(T+"/big.bin","rb").read()).digest()
check("download 300 KB matches", c==200 and same, f"{sd/1024:.0f} KB/s {e}")
c,b,*_=curl(B+"/st/small.txt"); check("download small", c==200 and b=="hello cube\n", repr(b))
c,b,*_=curl(B+"/st/small.txt","-H","X-Pin: "+pin,pin_=False); check("X-Pin header", c==200, c)
c,b,*_=curl(B+"/st/small.txt?pin="+pin,pin_=False); check("?pin= query", c==200, c)
c,b,*_=curl(B+"/st/nothing.txt"); check("missing file is 404", c==404, c)
c,b,*_=curl(B+"/st"); check("GET folder is 400", c==400, b.strip())
c,b,*_=curl("--path-as-is","-X","PUT","--data-binary","x",B+"/st/../evil.txt"); check("traversal rejected", c==400, c)
c,b,*_=curl("--path-as-is",B+"/st/%2e%2e/config.txt"); check("encoded traversal rejected", c==400, c)
c,b,*_=curl("-X","DELETE",B+"/st"); check("delete non-empty folder is 409", c==409, c)
for n in ("small.txt","big.bin","sub"):
    c,b,*_=curl("-X","DELETE",B+"/st/"+n); check("delete "+n, c==200, b.strip())
c,b,*_=curl("-X","DELETE",B+"/st/nothing"); check("delete missing is 404", c==404, c)
c,b,*_=curl("-X","DELETE",B+"/st"); check("delete folder", c==200, b.strip())
c,b,*_=curl(B+"/?list"); check("folder gone", c==200 and not any(e["n"]=="st" for e in json.loads(b)))
wrong="%04d"%((int(pin)+1)%10000)
codes=[curl(B+"/?list","-b","pin="+wrong,pin_=False)[0] for _ in range(5)]; check("wrong PIN is 401", codes==[401]*5, codes)
c,b,*_=curl(B+"/?list"); check("locked after 5 wrong PINs (429 even with the right one)", c==429, c)
read_for(31)
c,b,*_=curl(B+"/?list"); check("unlocked after 30 s", c==200, c)
t=cmd("app close",1.5)
log.close()
errs=[l for l in open(T+"/share_serial.log").read().splitlines() if re.search(r"<err>|FATAL",l)]
check("no errors on the serial log", not errs, errs[:3])
print("FAILED: "+", ".join(fails) if fails else "ALL PASSED")
sys.exit(1 if fails else 0)
