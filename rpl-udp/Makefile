CONTIKI_PROJECT = udp-client udp-server
all: $(CONTIKI_PROJECT)

# Which objective function this build runs (see project-conf.h):
#   0 = our energy-aware OF (default), 1 = MRHOF, 2 = OF0
# Use:  make TARGET=cooja EXPERIMENT_OF=1     (or export it before running
# Cooja). Objects are NOT rebuilt when it changes: "rm -rf build" first.
ifdef EXPERIMENT_OF
CFLAGS += -DEXPERIMENT_OF=$(EXPERIMENT_OF)
endif

CONTIKI=../..
include $(CONTIKI)/Makefile.include
