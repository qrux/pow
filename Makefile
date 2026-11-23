# pow: a live view of power, load, temperature and fans on Apple silicon. Written in C (src/).
#
#   make             build ./pow
#   make check       compare ./pow with the golden files: replays, drawn frames (text and LED images), a live run with --log
#   make bench       time drawing a big frame: images by zlib, by shared memory, and skipping unchanged charts
#   make install     build, then copy ./pow to ~/bin/pow
#   make uninstall   move ~/bin/pow aside to ~/bin/pow.removed, if it is ours
#   make clean       remove ./pow
#
# install refuses to replace a ~/bin/pow that is not an earlier pow (C or Python, or pm-chart.py, its old name),
# recognized by the first line of its help, old or new.

BIN   ?= $(HOME)/bin
NAME  ?= pow
DEST  := $(BIN)/$(NAME)
MARK  := pow: a live view of power
OLD   := pow: a top-style live view
OLDER := pm-chart.py: a top-style live view

SRC := src/main.c src/util.c src/data.c src/view.c src/term.c src/mac.c
HDR := src/pow.h src/help.inc
CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
ifeq ($(shell uname),Darwin)
LIBS := -framework CoreFoundation -framework IOKit -lz
else
LIBS := -lz -lm -lpthread
CFLAGS += -Wno-format-truncation
endif

.PHONY: all check bench install uninstall clean

all: pow

pow: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LIBS)

check: pow
	@LIBS="$(LIBS)" CC="$(CC)" bash test/check.sh

bench: pow
	@$(CC) -std=gnu11 -O2 -w -o test/c_frames test/c_frames.c src/util.c src/data.c src/view.c src/term.c src/mac.c $(LIBS)
	@python3 test/mkdata.py 7 > test/bench-data.txt
	@for m in px shm cache; do test/c_frames test/bench-data.txt $$m 50; done
	@rm -f test/c_frames test/bench-data.txt

install: pow
	@if [ -e "$(DEST)" ] && ! LC_ALL=C grep -a -q -e "$(MARK)" -e "$(OLD)" -e "$(OLDER)" "$(DEST)"; then echo "install: $(DEST) exists and is not pow; not replacing it"; exit 1; fi
	@mkdir -p "$(BIN)"
	@install -m 755 pow "$(DEST)"
	@echo "installed $(DEST)"
	@case ":$$PATH:" in *":$(BIN):"*) ;; *) echo "note: $(BIN) is not on your PATH";; esac

uninstall:
	@if [ -e "$(DEST)" ] && ! LC_ALL=C grep -a -q -e "$(MARK)" -e "$(OLD)" -e "$(OLDER)" "$(DEST)"; then echo "uninstall: $(DEST) is not pow; leaving it"; exit 1; fi
	@if [ -e "$(DEST)" ]; then mv "$(DEST)" "$(DEST).removed" && echo "moved $(DEST) to $(DEST).removed"; else echo "nothing at $(DEST)"; fi

clean:
	rm -f pow
