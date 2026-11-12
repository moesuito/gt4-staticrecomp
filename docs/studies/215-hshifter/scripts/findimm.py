import sys,struct
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
data=open(ELF,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata=True
text=data[OFF:OFF+TEXTSZ]
want=int(sys.argv[1],16)  # full address
hi=(want>>16)&0xffff; lo=want&0xffff
hits=[]
words=struct.unpack('<%dI'%(len(text)//4),text)
for idx,w in enumerate(words):
    if (w>>26)==0x0f and (w&0xffff)==hi:  # lui
        # next up to 8 instructions
        for j in range(1,10):
            w2=words[idx+j]
            op=w2>>26
            if op in (0x09,0x0d):  # addiu, ori
                if (w2&0xffff)==lo:
                    hits.append((BASE+idx*4,BASE+(idx+j)*4,w2>>26))
                    break
print('hits',len(hits))
for a,b,op in hits[:80]:
    print(hex(a),hex(b),'addiu' if op==9 else 'ori')
