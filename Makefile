# Makefile: A standard Makefile for

PKG_CONFIG_PATH:=/usr/lib/x86_64-linux-gnu/pkgconfig
export PKG_CONFIG_PATH

FFMPEG_LIBS   = libavformat libavcodec libavutil libswscale
FFMPEG_CFLAGS = $(shell /usr/bin/pkg-config --cflags $(FFMPEG_LIBS))
FFMPEG_LDLIBS = $(shell /usr/bin/pkg-config --libs $(FFMPEG_LIBS))

CC       = gcc
LD       = $(CC)
CFLAGS   = -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -O2
CFLAGS  += $(FFMPEG_CFLAGS)
LDFLAGS  = $(FFMPEG_LDLIBS)
INCDRS   =

all  : main

.c.o:
	$(CC) $(CFLAGS) -o $@ -c $(INCDRS) $<

main: main.o
	$(LD) -o $@ $^ $(LDFLAGS)

run: video2

video0: main
	./main /dev/video0
video2: main
	./main /dev/video2

clean:
	/bin/rm -rf main.o main

install-deps:
	@echo "Para instalar as dependências no Ubuntu/Debian:"
	@echo "sudo apt update"
	@echo "sudo apt install libavformat-dev libavcodec-dev libswscale-dev libavutil-dev v4l-utils"
	@echo ""
	@echo "Para instalar no CentOS/RHEL/Fedora:"
	@echo "sudo yum install ffmpeg-devel v4l-utils"
	@echo "ou"
	@echo "sudo dnf install ffmpeg-devel v4l-utils"

test-webcam:
	@echo "Testando dispositivos de vídeo disponíveis:"
	@ls -la /dev/video* 2>/dev/null || echo "Nenhum dispositivo encontrado"
	@echo ""
	@echo "Informações da webcam:"
	@v4l2-ctl --list-devices 2>/dev/null || echo "v4l-utils não instalado"
	@echo ""
	@echo "Formatos suportados:"
	@v4l2-ctl -d /dev/video0 --list-formats-ext 2>/dev/null || echo "Erro ao acessar /dev/video0"

# END OF FILE
