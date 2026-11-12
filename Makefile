PLUGIN    = clawsblink.so
PKGS      = claws-mail gtk+-3.0
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fPIC \
            $(shell pkg-config --cflags $(PKGS))
LDLIBS    = $(shell pkg-config --libs gtk+-3.0) -lm
PLUGINDIR = $(shell pkg-config --variable=plugindir claws-mail)
USERDIR   = $(HOME)/.claws-mail/plugins

all: $(PLUGIN)

$(PLUGIN): clawsblink.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS) $(LDLIBS)

# Per-user install (no root needed)
install-user: $(PLUGIN)
	install -d $(USERDIR)
	install -m 644 $(PLUGIN) $(USERDIR)/

# System-wide install
install: $(PLUGIN)
	install -d $(DESTDIR)$(PLUGINDIR)
	install -m 644 $(PLUGIN) $(DESTDIR)$(PLUGINDIR)/

uninstall-user:
	rm -f $(USERDIR)/$(PLUGIN)

uninstall:
	rm -f $(DESTDIR)$(PLUGINDIR)/$(PLUGIN)

clean:
	rm -f $(PLUGIN)

.PHONY: all install install-user uninstall uninstall-user clean
