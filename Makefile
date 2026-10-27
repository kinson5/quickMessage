CC_MAC = clang
CC_WIN = i686-w64-mingw32-gcc
WINDRES = i686-w64-mingw32-windres

CFLAGS = -O2 -Wall

all: mac win

mac:
	$(CC_MAC) $(CFLAGS) -arch arm64 -arch x86_64 -o quickMessage_mac main.c qrcodegen.c -lpthread

win: app_res.o
	$(CC_WIN) $(CFLAGS) -static -Wl,--subsystem,console:5.01 -o quickMessage_win.exe main.c qrcodegen.c app_res.o -lws2_32 -luser32 -lshell32 -lkernel32

app_res.o: app.rc icon.ico
	$(WINDRES) app.rc -O coff -o app_res.o

clean:
	rm -f quickMessage_mac quickMessage_win.exe app_res.o
