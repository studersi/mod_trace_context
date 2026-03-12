APXS ?= apxs
HTTPD_SRC ?= ../httpd

MODULE = mod_trace_context.la
SOURCE = mod_trace_context.c
EXTRA_CFLAGS = -I$(HTTPD_SRC)/modules/loggers -Wall -Wextra -Werror

.PHONY: all install clean

all:
	$(APXS) -c -Wc,"$(EXTRA_CFLAGS)" $(SOURCE)

install: all
	$(APXS) -i -a -n trace_context $(MODULE)

clean:
	rm -rf .libs *.la *.lo *.slo *.o *.so *.lai
