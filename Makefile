CC=gcc
CFLAGS=-Wall -g `xml2-config --cflags`
LIBS=-L. -luu `xml2-config --libs` -lpthread -lm
INCLUDES=-I. -I/usr/include/libxml2
OBJS=nzbnews.o
TARGET=nzbnews

all:	$(TARGET)

nzbnews:	$(OBJS) Makefile
	$(CC) $(CFLAGS) $(LIBS) $(OBJS) -o nzbnews -luu

%.o:	%.c Makefile
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

tags:
	cscope -b
	ctags -R *.[ch]
	
clean:
	rm -f $(OBJS) $(TARGET)
