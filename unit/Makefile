# -*- Mode: makefile-gmake -*-

all:
%:
	@$(MAKE) -C nci_core $*
	@$(MAKE) -C nci_sar $*
	@$(MAKE) -C nci_sm $*

clean: unitclean
	rm -f *~
	rm -f common/*~
	rm -f coverage/*~
	rm -f coverage/*.gcov
	rm -fr coverage/report
