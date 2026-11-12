import sys,struct
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
data=open(ELF,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata=True
vals=set(int(x,16) for x in sys.argv[1:])
for i in md.disasm(data[OFF:OFF+TEXTSZ],BASE):
    if i.mnemonic in ('addiu','ori'):
        ops=[x.strip() for x in i.op_str.split(',')]
        if len(ops)==3:
            try: v=int(ops[2],16)
            except: continue
            if v in vals:
                print(f'{i.address:08x}: {i.mnemonic:6s} {i.op_str}')
