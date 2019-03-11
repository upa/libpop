

subdirs = kmod lib tools test

all:
	@(for d in $(subdirs); do $(MAKE) -C $$d; done)
clean:
	@(for d in $(subdirs); do $(MAKE) -C $$d clean; done)

remake: clean all
