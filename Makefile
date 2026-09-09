CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
CPPFLAGS := -Isrc -Ithird_party
LDLIBS  := -lcurl

BIN      := blikk-attest-check
SRC_DIR  := src
TP_DIR   := third_party
OBJ_DIR  := build

SRCS := $(wildcard $(SRC_DIR)/*.c) $(TP_DIR)/cJSON.c
OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(notdir $(SRCS)))
VPATH := $(SRC_DIR):$(TP_DIR)

.PHONY: all clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN)
