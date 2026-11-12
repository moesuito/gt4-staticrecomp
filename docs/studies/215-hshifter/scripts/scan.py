import sys,struct,collections
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
data=open(ELF,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata=True
text=data[OFF:OFF+TEXTSZ]
insns=[]
jal=collections.defaultdict(list)
for i in md.disasm(text,BASE):
    insns.append((i.address,i.mnemonic,i.op_str,i.bytes))
    if i.mnemonic=='jal':
        try: t=int(i.op_str,16)
        except: continue
        jal[t].append(i.address)
targets=[int(x,16) for x in sys.argv[1:]]
for t in targets:
    print(hex(t),'callers:',[hex(a) for a in jal.get(t,[])])
import pickle
with open('insns.pkl','wb') as f: pickle.dump(insns,f)
print('insns',len(insns))
