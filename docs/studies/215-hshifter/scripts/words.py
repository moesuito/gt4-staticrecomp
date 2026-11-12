import sys,struct
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
d=open(ELF,'rb').read()
BASE=0x100000; OFF=0x1000
for a in sys.argv[1:]:
    addr=int(a,16); off=OFF+addr-BASE
    w=struct.unpack_from('<I',d,off)[0]
    print(hex(addr),hex(w),d[off:off+4].hex())
