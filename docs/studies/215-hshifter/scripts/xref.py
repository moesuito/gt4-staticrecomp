import sys,struct
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
data=open(ELF,'rb').read()
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
DSEG_OFF=0x518A80; DSEG_VADDR=0x617A80; DSIZE=0x0BE37C
def find_val(val):
    pat=struct.pack('<I',val)
    out=[]
    # in data segment
    d=data[DSEG_OFF:DSEG_OFF+DSIZE]
    i=d.find(pat)
    while i>=0:
        out.append(DSEG_VADDR+i); i=d.find(pat,i+1)
    # in text: lui+addiu/ori pair value (only approximate by immediate halves)
    hi=(val>>16)&0xffff; lo=val&0xffff
    return out
for v in sys.argv[1:]:
    print(hex(int(v,16)), [hex(x) for x in find_val(int(v,16))])
