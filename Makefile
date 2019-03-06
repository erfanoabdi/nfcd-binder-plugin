# -*- Mode: makefile-gmake -*-

.PHONY: clean all release
.PHONY: nci_debug_lib nci_release_lib

#
# Required packages
#

LDPKGS = libgbinder libglibutil gobject-2.0 glib-2.0
PKGS = $(LDPKGS) nfcd-plugin

#
# Default target
#

all: debug release

#
# Library name
#

NAME = binder
LIB_NAME = $(NAME)
LIB_SONAME = $(LIB_NAME).so
LIB = $(LIB_SONAME)

#
# libnfc-nci
#

NCI_LIB = libnci.a
NCI_DIR = nci
NCI_BUILD_DIR = $(NCI_DIR)/build
NCI_DEBUG_LIB = $(NCI_BUILD_DIR)/debug/$(NCI_LIB)
NCI_RELEASE_LIB = $(NCI_BUILD_DIR)/release/$(NCI_LIB)

#
# Sources
#

SRC = \
  binder_nfc_adapter.c \
  binder_nfc_plugin.c \
  binder_nfc_target.c

#
# Directories
#

SRC_DIR = src
BUILD_DIR = build
DEBUG_BUILD_DIR = $(BUILD_DIR)/debug
RELEASE_BUILD_DIR = $(BUILD_DIR)/release

NCI_DIR = nci
NCI_SRC_DIR = $(NCI_DIR)/src
NCI_CFLAGS = -I$(NCI_SRC_DIR)

#
# Tools and flags
#

CC = $(CROSS_COMPILE)gcc
LD = $(CC)
WARNINGS = -Wall
BASE_FLAGS = -fPIC -fvisibility=hidden
DEFINES = -DNFC_PLUGIN_EXTERNAL
FULL_CFLAGS = $(BASE_FLAGS) $(CFLAGS) $(DEFINES) $(WARNINGS) -MMD -MP \
  -I$(NCI_DIR)/include $(shell pkg-config --cflags $(PKGS))
FULL_LDFLAGS = $(BASE_FLAGS) $(LDFLAGS) -shared
DEBUG_FLAGS = -g
RELEASE_FLAGS =

ifndef KEEP_SYMBOLS
KEEP_SYMBOLS = 0
endif

ifneq ($(KEEP_SYMBOLS),0)
RELEASE_FLAGS += -g
endif

DEBUG_LDFLAGS = $(FULL_LDFLAGS) $(DEBUG_FLAGS)
RELEASE_LDFLAGS = $(FULL_LDFLAGS) $(RELEASE_FLAGS)
DEBUG_CFLAGS = $(FULL_CFLAGS) $(DEBUG_FLAGS) -DDEBUG
RELEASE_CFLAGS = $(FULL_CFLAGS) $(RELEASE_FLAGS) -O2

LIBS = $(shell pkg-config --libs $(LDPKGS))
DEBUG_LIBS = $(NCI_DEBUG_LIB) $(LIBS)
RELEASE_LIBS = $(NCI_RELEASE_LIB) $(LIBS)

#
# Files
#

DEBUG_OBJS = \
  $(SRC:%.c=$(DEBUG_BUILD_DIR)/%.o) \
  $(NCI_SRC:%.c=$(DEBUG_BUILD_DIR)/%.o)
RELEASE_OBJS = \
  $(SRC:%.c=$(RELEASE_BUILD_DIR)/%.o) \
  $(NCI_SRC:%.c=$(RELEASE_BUILD_DIR)/%.o)

#
# Dependencies
#

DEPS = \
  $(DEBUG_OBJS:%.o=%.d) \
  $(RELEASE_OBJS:%.o=%.d)
ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(DEPS)),)
-include $(DEPS)
endif
endif

DEBUG_DEPS = $(NCI_DEBUG_LIB)
RELEASE_DEPS = $(NCI_RELEASE_LIB)

$(NCI_DEBUG_LIB): | nci_debug_lib
$(NCI_RELEASE_LIB): | nci_release_lib
$(DEBUG_OBJS): | $(DEBUG_BUILD_DIR)
$(RELEASE_OBJS): | $(RELEASE_BUILD_DIR)

#
# Rules
#

DEBUG_LIB = $(DEBUG_BUILD_DIR)/$(LIB)
RELEASE_LIB = $(RELEASE_BUILD_DIR)/$(LIB)

debug: nci_debug_lib $(DEBUG_LIB)

release: nci_release_lib $(RELEASE_LIB)

clean:
	make -C $(NCI_DIR) clean
	rm -f *~ rpm/*~ $(SRC_DIR)/*~
	rm -fr $(BUILD_DIR)

nci_debug_lib:
	make -C $(NCI_DIR) debug

nci_release_lib:
	make -C $(NCI_DIR) release

$(DEBUG_BUILD_DIR):
	mkdir -p $@

$(RELEASE_BUILD_DIR):
	mkdir -p $@

$(DEBUG_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(RELEASE_BUILD_DIR)/%.o : $(SRC_DIR)/%.c
	$(CC) -c $(RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(DEBUG_BUILD_DIR)/%.o : $(NCI_SRC_DIR)/%.c
	$(CC) -c $(NCI_CFLAGS) $(DEBUG_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(RELEASE_BUILD_DIR)/%.o : $(NCI_SRC_DIR)/%.c
	$(CC) -c $(NCI_CFLAGS) $(RELEASE_CFLAGS) -MT"$@" -MF"$(@:%.o=%.d)" $< -o $@

$(DEBUG_LIB): $(DEBUG_OBJS) $(DEBUG_DEPS)
	$(LD) $(DEBUG_OBJS) $(DEBUG_LDFLAGS) $(DEBUG_LIBS) -o $@

$(RELEASE_LIB): $(RELEASE_OBJS) $(RELEASE_DEPS)
	$(LD) $(RELEASE_OBJS) $(RELEASE_LDFLAGS) $(RELEASE_LIBS) -o $@

#
# Install
#

INSTALL_PERM  = 755
INSTALL = install
INSTALL_DIRS = $(INSTALL) -d
INSTALL_FILES = $(INSTALL) -m $(INSTALL_PERM)
INSTALL_LIB_DIR = $(DESTDIR)/usr/lib/nfcd/plugins

install: $(INSTALL_LIB_DIR)
	$(INSTALL_FILES) $(RELEASE_LIB) $(INSTALL_LIB_DIR)

$(INSTALL_LIB_DIR):
	$(INSTALL_DIRS) $@
