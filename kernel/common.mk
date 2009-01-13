# Makefile fragment for non-Kbuild part of kernel makefiles

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
KERN_RELEASE := $(shell grep -s UTS_RELEASE \
		$(KERNELDIR)/include/linux/utsrelease.h | cut -f2 -d\")

.PHONY: module
module: check_config $(MOD_DEPENDS)
	@$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

.PHONY: clean
clean: check_config
	@$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean


.PHONY: install
# Recent kernels automatically run depmod.  If we're using a DESTDIR,
# we don't want the depmod files installed, but if we're not (or the DESTDIR
# is a chroot or somesuch), we don't want to delete them.  So, we only delete
# them if they weren't there before we started.
install: NEED_CLEAR := $(shell [ -f \
			$(DESTDIR)/lib/modules/$(KERN_RELEASE)/modules.dep ] \
			|| echo 1)
install: check_config module
	@$(MAKE) -C $(KERNELDIR) M=$(CURDIR) INSTALL_MOD_PATH=$(DESTDIR) \
		INSTALL_MOD_DIR=openisr modules_install
	@[ "$(NEED_CLEAR)" = "1" ] && \
		rm -f $(DESTDIR)/lib/modules/$(KERN_RELEASE)/modules.* || true


.PHONY: check_config
check_config:
ifeq ($(KERN_RELEASE),)
	@echo "Kernel tree at $(KERNELDIR) not configured"
	@exit 1
endif