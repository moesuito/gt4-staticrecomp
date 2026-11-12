import sys,struct
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000
data=open(ELF,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata=True
addr=int(sys.argv[1],16)
code=data[OFF+addr-BASE:OFF+addr-BASE+0x80]
for i in md.disasm(code,addr): print(f'{i.address:08x}: {i.mnemonic:8s} {i.op_str}')
