# Makefile for vips plugin 

# object files
OBJS = kakadu-vips.o kakaduload.o

# source files
SRC = $(OBJS:.o=.cpp) 

# headers
HEADERS = kakadu.h

$(SRC): $(HEADERS)

ALL = $(SRC) $(HEADERS)

CXXFLAGS = 

CXXFLAGS += -shared -fPIC 
CPPFLAGS += `pkg-config vips --cflags`
LDFLAGS += `pkg-config vips --libs`

# find kakadu headers and libs ... use the static libs to make install
# easier
KAKADUHOME = $(shell echo ../kakadu/v* | sed 's/ /\n/g' | grep -v .zip)
KAKADU_ARCH = Linux-x86-64-gcc
CPPFLAGS += -I$(KAKADUHOME)/managed/all_includes
LDFLAGS += $(KAKADUHOME)/lib/$(KAKADU_ARCH)/libkdu_aux.a
LDFLAGS += $(KAKADUHOME)/lib/$(KAKADU_ARCH)/libkdu.a

OUT = vips-kakadu.so

all: $(OUT)
release: $(OUT)
debug: $(OUT)

.PHONY: debug  
debug: CXXFLAGS += -g -Wall

.PHONY: release  
release: CXXFLAGS += -O3

$(OUT): $(OBJS)
# must link C++ plugins with the C++ compiler
	$(CXX) -o $(OUT) -shared $(OBJS) $(LDFLAGS)

clean: 
	$(RM) $(OBJS) 

tags: $(ALL)
	ctags $(ALL)

# version as MAJOR.MINOR
VIPS_VERSION = $(shell pkg-config vips --modversion | \
	         awk '{split($$1,a,"."); print a[1]"."a[2]}')
PLUGIN_DIR = $(VIPSHOME)/lib/vips-modules-$(VIPS_VERSION)

install: $(OUT)
	-mkdir -p $(PLUGIN_DIR)
	-cp $(OUT) $(PLUGIN_DIR)

