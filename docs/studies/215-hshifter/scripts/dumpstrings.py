import sys
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
data=open(ELF,'rb').read()
DSEG_OFF=0x518A80; DSEG_VADDR=0x617A80
start=int(sys.argv[1],16); n=int(sys.argv[2],0)
off=DSEG_OFF+start-DSEG_VADDR
b=data[off:off+n]
cur=b''
for i,ch in enumerate(b):
    if 32<=ch<127: cur+=bytes([ch])
    else:
        if len(cur)>=4: print(hex(start+i-len(cur)),cur.decode())
        cur=b''
if len(cur)>=4: print(hex(start+n-len(cur)),cur.decode())
