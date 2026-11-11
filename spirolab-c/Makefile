CC ?= cc
PACKAGES = gtk+-3.0 pangocairo cairo-svg cairo-pdf libonnxruntime epoxy
CPPFLAGS += $(shell pkg-config --cflags $(PACKAGES))
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PACKAGES)) -lm
SOURCES = desktop.c render.c engine.c trace.c ai.c document.c image.c spiro3d.c spiro3d_io.c viewer3d.c gpu3d.c
OBJECTS = $(SOURCES:.c=.o)
all: spirolab
spirolab: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)
$(OBJECTS): spiro.h render.h ai.h document.h image.h spiro3d.h spiro3d_io.h viewer3d.h gpu3d.h
clean:
	rm -f $(OBJECTS) spirolab
.PHONY: all clean

test: spirolab
	./tests/run.sh
.PHONY: test
