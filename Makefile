CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
WARN = -Wall -Wextra -Wpedantic -Wshadow -Wno-sign-conversion
BUILD ?= build

SNOVAC_DIR ?= ../snovac
SNOVAC_LIB ?= $(SNOVAC_DIR)/build/libsnovart.a
SNOVAC_CHECK_OBJ ?= $(SNOVAC_DIR)/build/cmd_check.o

INCLUDES = -Isrc -I$(SNOVAC_DIR)

ifeq ($(OS),Windows_NT)
  EXE := .exe
else
  EXE :=
endif

BIN = $(BUILD)/snova-lsp$(EXE)
TEST_BIN = $(BUILD)/test_completion$(EXE)

SRCS = src/json.c src/lsp_transport.c src/lsp_document.c src/lsp_analysis.c \
       src/lsp_definition.c src/lsp_hover.c src/lsp_symbols.c src/lsp_completion.c \
       src/lsp_code_action.c src/main.c

OBJS = $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin

.PHONY: all clean test install

all: $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(SNOVAC_LIB):
	$(MAKE) -C $(SNOVAC_DIR) all

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -c -o $@ $<

$(BIN): $(OBJS) $(SNOVAC_LIB)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SNOVAC_CHECK_OBJ) $(SNOVAC_LIB)

$(TEST_BIN): tests/test_completion.c $(filter-out $(BUILD)/main.o,$(OBJS)) $(SNOVAC_LIB) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(INCLUDES) -o $@ tests/test_completion.c $(filter-out $(BUILD)/main.o,$(OBJS)) $(SNOVAC_CHECK_OBJ) $(SNOVAC_LIB)

test: $(TEST_BIN)
	./$(TEST_BIN)

install: $(BIN)
	@mkdir -p $(BINDIR)
	install -m 755 $(BIN) $(BINDIR)/snova-lsp$(EXE)
	ln -sf $(BINDIR)/snova-lsp$(EXE) $(BINDIR)/snovalang-lsp$(EXE)
	@echo "✓ Installed snova-lsp and snovalang-lsp to $(BINDIR)"

clean:
	rm -rf $(BUILD)
