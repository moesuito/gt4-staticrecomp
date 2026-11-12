import sys,struct,collections
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
data=open(ELF,'rb').read()
text=data[OFF:OFF+TEXTSZ]
lo=int(sys.argv[1],16); hi=int(sys.argv[2],16)
t=collections.Counter()
for off in range(0,len(text)-3,4):
    w=struct.unpack_from('<I',text,off)[0]
    if w>>26==3:
        pc=BASE+off
        target=((pc+4)&0xF0000000)|((w&0x3FFFFFF)<<2)
        if lo<=target<hi: t[target]+=1
for a in sorted(t): print(hex(a),t[a])
