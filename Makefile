# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright 2001-2004, ps2dev - http://www.ps2dev.org
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.

EE_BIN = ViGEmClientPs2.elf
EE_OBJS = ps2ip.o DEV9_irx.o NETMAN_irx.o SMAP_irx.o
EE_LIBS = -lnetman -lps2ip -ldebug -lpatches -lpadx

all: $(EE_BIN)  sio2man.irx padman.irx

clean:
	rm -f $(EE_BIN) $(EE_OBJS) DEV9_irx.c NETMAN_irx.c SMAP_irx.c sio2man.irx padman.irx 

DEV9_irx.c: $(PS2SDK)/iop/irx/ps2dev9.irx
	bin2c $< DEV9_irx.c DEV9_irx

NETMAN_irx.c: $(PS2SDK)/iop/irx/netman.irx
	bin2c $< NETMAN_irx.c NETMAN_irx

SMAP_irx.c: $(PS2SDK)/iop/irx/smap.irx
	bin2c $< SMAP_irx.c SMAP_irx

sio2man.irx:
	cp $(PS2SDK)/iop/irx/sio2man.irx $@

padman.irx:
	cp $(PS2SDK)/iop/irx/padman.irx $@

run: $(EE_BIN)
	ps2client execee host:$(EE_BIN) free

reset:
	ps2client reset

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal

