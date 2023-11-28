# Makefile for vips plugin 

# object files
OBJS = kakadu-vips.o kakaduload.o

# source files
SRC = $(BASEOBJS:.o=.cpp) 

# headers
HEADERS = kakadu.h

$(SRC): $(HEADERS)

ALL = $(SRC) $(HEADERS)

CXXFLAGS = 

CXXFLAGS += -shared -fPIC 
CPPFLAGS += `pkg-config vips --cflags`
LDFLAGS += `pkg-config vips --libs`

# find kakadu headers and lib
KAKADUHOME = $(shell echo ../kakadu/v* | sed 's/ /\n/g' | grep -v .zip)
KAKADU_ARCH = Linux-x86-64-gcc
KAKADU_VERSION = 83R
KAKADU_LIB = kdu_v$(KAKADU_VERSION)
KAKADU_AUX_LIB = kdu_a$(KAKADU_VERSION)
CPPFLAGS += -I$(KAKADUHOME)/coresys/common 
CPPFLAGS += -I$(KAKADUHOME)/managed/all_includes
LDFLAGS += -L$(KAKADUHOME)/lib/$(KAKADU_ARCH)
LDFLAGS += -l$(KAKADU_LIB) -l$(KAKADU_AUX_LIB)

OUT = vips-kakadu.so

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

