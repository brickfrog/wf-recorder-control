CC ?= cc
CFLAGS += -O2 -Wall -Wextra -Wno-deprecated-declarations $(shell pkg-config --cflags gtk4)
LDLIBS += $(shell pkg-config --libs gtk4)

wf-recorder-control: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

.PHONY: clean
clean:
	rm -f wf-recorder-control
