import sys,struct
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
d=open(ELF,'rb').read()
addr=0x37ACD0
b=d[0x1000+addr-0x100000:0x1000+addr-0x100000+64]
print(b.hex(), struct.unpack('<16I',b))
for mode in [capstone.CS_MODE_MIPS64, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN, capstone.CS_MODE_MIPS64+capstone.CS_MODE_BIG_ENDIAN]:
    md=capstone.Cs(capstone.CS_ARCH_MIPS, mode)
    md.skipdata=True
    print('mode',mode)
    for i in md.disasm(b,addr):
        print(f'{i.address:08x}: {i.bytes.hex():8s} {i.mnemonic:8s} {i.op_str}')
