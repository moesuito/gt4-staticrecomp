import sys, struct
sys.path.insert(0, r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000
with open(ELF,'rb') as f:
    data=f.read()
def read_guest(addr,n):
    return data[OFF+(addr-BASE):OFF+(addr-BASE)+n]
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32+capstone.CS_MODE_LITTLE_ENDIAN)
md.detail=False
md.skipdata=True
start=int(sys.argv[1],16); stop=int(sys.argv[2],16)
code=read_guest(start,stop-start)
for i in md.disasm(code,start):
    print(f'{i.address:08x}: {i.bytes.hex():8s} {i.mnemonic:8s} {i.op_str}')
