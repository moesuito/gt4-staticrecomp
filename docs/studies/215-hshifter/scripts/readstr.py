import sys,struct
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
data=open(ELF,'rb').read()
DSEG_OFF=0x518A80; DSEG_VADDR=0x617A80
for arg in sys.argv[1:]:
    addr=int(arg,16)
    off=DSEG_OFF+addr-DSEG_VADDR
    b=data[off:off+128].split(b'\0')[0]
    print(hex(addr),repr(b.decode('latin1')))
