import sys,struct
sys.path.insert(0,r'O:\tmp-capstone-lib')
import capstone
ELF=r'C:\Antigravity\gt4-staticrecomp\private\reconstructed\SCUS_973.28.elf'
BASE=0x100000; OFF=0x1000; TEXTSZ=0x517A14
data=open(ELF,'rb').read()
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64+capstone.CS_MODE_LITTLE_ENDIAN)
md.skipdata=True
text=data[OFF:OFF+TEXTSZ]
offs=[int(x,16) for x in sys.argv[1:]]
seq=list(md.disasm(text,BASE))
# find lw reg,0x14(base) then addiu reg,reg,off within 6
for i,ins in enumerate(seq):
    if ins.mnemonic=='lw' and '0x14(' in ins.op_str:
        try:
            parts=ins.op_str.split(',')
            reg=parts[0].strip()
        except: continue
        for j in range(i+1,min(i+7,len(seq))):
            s=seq[j]
            if s.mnemonic=='addiu' and s.op_str.startswith(reg+',') and s.op_str.endswith(','):
                pass
            if s.mnemonic=='addiu':
                ops=[x.strip() for x in s.op_str.split(',')]
                if len(ops)==3 and ops[0]==reg and ops[1]==reg:
                    try: val=int(ops[2],16)
                    except: continue
                    if val in offs:
                        print(hex(seq[i].address),'lw14 ->',hex(s.address),'off',hex(val))
                    break
