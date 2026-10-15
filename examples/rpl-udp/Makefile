
CONTIKI_PROJECT = udp-client udp-server
all: $(CONTIKI_PROJECT)

CONTIKI=../..
include $(CONTIKI)/Makefile.include

# Select the RPL objective function for this experiment
ifeq ($(EXPERIMENT_OF),1)
CFLAGS += -DEXPERIMENT_OF=1
else ifeq ($(EXPERIMENT_OF),2)
CFLAGS += -DEXPERIMENT_OF=2
else
CFLAGS += -DEXPERIMENT_OF=0
endif
