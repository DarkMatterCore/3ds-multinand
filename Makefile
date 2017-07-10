# This Makefile will build the MinGW Win32 application.

HEADERS = include/resource.h include/3ds-multinand.h
OBJS = obj/winmain.o obj/resource.o obj/fat32format.o obj/3ds-multinand.o
INCLUDE_DIRS = -I.\include

CC = gcc
LDFLAGS = -s -Wl,--subsystem,windows -lcomctl32 -lcomdlg32 -lgdi32
RC = windres

#FIX: Compile with TDM-GCC instead of MinGW to avoid problems with _wfopen while using -std=c99
CFLAGS = -Os -std=c99 -D UNICODE -D _UNICODE -D NTDDI_VERSION=0x05010000 -D _WIN32_WINNT=0x0501 -D _WIN32_IE=0x0500 -D WINVER=0x0500 -Wall

EXE_EXT = .exe
PROJECT_NAME = 3ds-multinand
EXE_NAME = $(PROJECT_NAME)$(EXE_EXT)

all: $(EXE_NAME)

$(EXE_NAME): ${OBJS}
	${CC} -o "$@" ${OBJS} ${LDFLAGS}

clean:
	rm -f obj/*.o "$(EXE_NAME)"

obj/%.o: src/%.c ${HEADERS}
	${CC} ${CFLAGS} ${INCLUDE_DIRS} -c $< -o $@

obj/resource.o: res/resource.rc res/$(PROJECT_NAME).manifest res/$(PROJECT_NAME).ico include/resource.h
	${RC} ${INCLUDE_DIRS} -I.\res -i $< -o $@