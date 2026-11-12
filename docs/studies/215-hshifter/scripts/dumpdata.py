import sys,struct
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
data=open(ELF,'rb').read()
DSEG_OFF=0x518A80; DSEG_VADDR=0x617A80; DSIZE=0x0BE37C
def rd(addr,n):
    return data[DSEG_OFF+addr-DSEG_VADDR:DSEG_OFF+addr-DSEG_VADDR+n]
start=int(sys.argv[1],16); stop=int(sys.argv[2],16)
for a in range(start,stop,4):
    v=struct.unpack('<I',rd(a,4))[0]
    print(f'{a:08x}: {v:08x}')
