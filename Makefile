

subdirs = kmod lib tools test


install = /usr/bin/install -m 644 -D 
install_h = include/libpop.h
install_l = lib/libpop.a
install_k = kmod/boogiepop.ko
kmod_dir = /lib/modules/$(shell uname -r)/extra/

all:
	@(for d in $(subdirs); do $(MAKE) -C $$d; done)
clean:
	@(for d in $(subdirs); do $(MAKE) -C $$d clean; done)

install: all
	$(install) $(install_h) /usr/local/include/
	$(install) $(install_l) /usr/local/lib/
	$(install) $(install_k) $(kmod_dir)/
	/sbin/depmod

remake: clean all
