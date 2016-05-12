#
# $Id: Makefile,v 1.14 2007/05/29 08:04:46 cadm Exp $
#

CC	= cc -g
CFLAGS	= -Wall -Wstrict-prototypes

.if defined(WITH_DEBUG)
CFLAGS  += -DWITH_DEBUG
.endif

PREFIX	= /usr/local

DATAPIPE_OBJ	= datapipe.o
DATAPIPEC_OBJ    = datapipec.o base64.o

BIO_DATAPIPE_OBJ= bio-datapipe.o
PB_OBJ		= pb.o

OUTPUT_PROGS	= datapipe datapipec
PROGS		= $(OUTPUT_PROGS)

all: $(PROGS)

static:
	$(MAKE) CC='cc -g -static' all

install: $(OUTPUT_PROGS)
	$(INSTALL) -c -m 555 $(OUTPUT_PROGS) $(PREFIX)/sbin

datapIpe: $(DATAPIPE_OBJ)
	$(CC) $(CFLAGS) -o $@ $(DATAPIPE_OBJ)
datapipec: $(DATAPIPEC_OBJ)
	$(CC) $(CFLAGS) -o $@ $(DATAPIPEC_OBJ)

bio-datapipe: $(BIO_DATAPIPE_OBJ)
	$(CC) $(CFLAGS) -o $@ $(BIO_DATAPIPE_OBJ) -lssl -lcrypto
pb:	$(PB_OBJ)
	$(CC) $(CFLAGS) -o $@ $(PB_OBJ) -lssl -lcrypto


tags:	TAGS

TAGS:	*.c
	etags *.c

clean:
	rm -f *.o *.core core *~ *.gmon TAGS tags $(PROGS)

datapipe.o: datapipe.c
	$(CC) -c -o $@ $(CFLAGS) -DDYNAMIC_CLIENTS=1 datapipe.c

conf-file.o: conf-file.c conf-file.h

decrypt.o: decrypt.c

datapipec.o: datapipec.c

base64.o:	base64.c

bio-datapipe.o: bio-datapipe.c

pb.o: pb.c

TARBALLFILES	= *.c *.h Makefile datapipe.conf.sample CHANGELOG
tarball:
	- rm -rf ./datapipe2
	mkdir datapipe2
	for i in $(TARBALLFILES) ; do ln -s ../$$i datapipe2 ; done
	tar cvyLf datapipe2.tar.bz datapipe2
	rm -rf ./datapipe2
