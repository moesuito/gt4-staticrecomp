import sys,struct
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
data=open(ELF,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata=True
offs=set(int(x,16) for x in sys.argv[1:])
for i in md.disasm(data[OFF:OFF+TEXTSZ],BASE):
    op=i.op_str
    if '(' in op:
        try:
            disp=op.split(',')[-1].strip()
            if disp.startswith('0x') and '(' in disp:
                val=int(disp.split('(')[0],16)
                if val in offs and i.mnemonic in ('lb','lbu','sb','lh','lhu','sh','lw','sw','lwc1','swc1','ld','sd'):
                    print(f'{i.address:08x}: {i.mnemonic:6s} {op}')
        except Exception: pass
