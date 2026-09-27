CC ?= cc
CFLAGS += -O2 -Wall -Wextra -Wno-deprecated-declarations $(shell pkg-config --cflags gtk4 libportal-gtk4 gstreamer-1.0 gstreamer-app-1.0 libpipewire-0.3 libavcodec)
LDLIBS += $(shell pkg-config --libs gtk4 libportal-gtk4 gstreamer-1.0 gstreamer-app-1.0 libpipewire-0.3 libavcodec)

wf-recorder-control: main.c window_capture.c window_capture.h portal_pipewire.c portal_pipewire.h encoding.c encoding.h
	$(CC) $(CFLAGS) -o $@ main.c window_capture.c portal_pipewire.c encoding.c $(LDLIBS)

test-encoding: test_encoding.c encoding.c encoding.h
	$(CC) $(CFLAGS) -o $@ test_encoding.c encoding.c $(LDLIBS)

.PHONY: check
check: test-encoding
	./test-encoding

.PHONY: clean
clean:
	rm -f wf-recorder-control test-encoding
