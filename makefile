ROOT_DIR  = $(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
SRC_DIR   = $(ROOT_DIR)/src
BUILD_DIR = $(ROOT_DIR)/build

CC       ?= cc
CFLAGS    = -std=c99 -pedantic -Wall -Wextra -Wno-unused-parameter -g -O2 -D_POSIX_C_SOURCE=200809L
INC       = -I$(SRC_DIR)

SRC = $(wildcard $(SRC_DIR)/*.c)
OBJ = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRC))
PIC_OBJ = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/pic/%.o, $(SRC))

STATIC_LIB = $(BUILD_DIR)/libaudiotag.a
SHARED_LIB = $(BUILD_DIR)/libaudiotag.so

SAMPLE_EXTRA = $(ROOT_DIR)/sample/file_audio_stream.c
SAMPLE_SRC = $(filter-out $(SAMPLE_EXTRA), $(wildcard $(ROOT_DIR)/sample/*.c))
SAMPLE_BIN = $(patsubst $(ROOT_DIR)/sample/%.c, $(BUILD_DIR)/sample/%, $(SAMPLE_SRC))

TEST_SRC = $(wildcard $(ROOT_DIR)/test/*.c)
TEST_BIN = $(patsubst $(ROOT_DIR)/test/%.c, $(BUILD_DIR)/test/%, $(TEST_SRC))

COV_DIR = $(BUILD_DIR)/coverage
COV_BIN = $(COV_DIR)/tests

all: $(STATIC_LIB) $(SHARED_LIB) $(SAMPLE_BIN)

# Static library
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "[C] $(notdir $<)"
	@mkdir -p $(BUILD_DIR)
	$(CC) -c $(CFLAGS) $(INC) -o $@ $<

$(STATIC_LIB): $(OBJ)
	@echo "[AR] $(notdir $@)"
	$(AR) rcs $@ $^

# Shared library
$(BUILD_DIR)/pic/%.o: $(SRC_DIR)/%.c
	@echo "[C-PIC] $(notdir $<)"
	@mkdir -p $(BUILD_DIR)/pic
	$(CC) -c $(CFLAGS) -fPIC -fvisibility=hidden $(INC) -o $@ $<

$(SHARED_LIB): $(PIC_OBJ)
	@echo "[SO] $(notdir $@)"
	$(CC) -shared $(CFLAGS) -o $@ $^

# Sample programs
$(BUILD_DIR)/sample/%: $(ROOT_DIR)/sample/%.c $(SAMPLE_EXTRA) $(STATIC_LIB)
	@echo "[SAMPLE] $(notdir $@)"
	@mkdir -p $(BUILD_DIR)/sample
	$(CC) $(CFLAGS) $(INC) -I$(ROOT_DIR)/sample -o $@ $< $(SAMPLE_EXTRA) $(STATIC_LIB)

# Test programs
$(BUILD_DIR)/test/%: $(ROOT_DIR)/test/%.c $(STATIC_LIB)
	@echo "[TEST] $(notdir $@)"
	@mkdir -p $(BUILD_DIR)/test
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(STATIC_LIB)

test: $(TEST_BIN)
	@echo "=== Unit tests ==="
	$(BUILD_DIR)/test/tests

# Coverage: reuse CFLAGS but swap -O2 for -O0 and add gcov flags
COV_CFLAGS = $(subst -O2,-O0,$(CFLAGS)) --coverage

$(COV_BIN): $(TEST_SRC) $(SRC)
	@echo "[COV] building with coverage"
	@mkdir -p $(COV_DIR)
	$(CC) $(COV_CFLAGS) $(INC) -o $@ $^

coverage: $(COV_BIN)
	@echo "=== Unit tests (coverage build) ==="
	@cd $(COV_DIR) && ./tests
	@echo ""
	@echo "=== Coverage summary ==="
	@cd $(COV_DIR) && gcov -n *.gcda 2>&1 \
		| grep -E "^File '.*/src/|^Lines executed" \
		| sed "s|^File '$(ROOT_DIR)/src/||; s|'$$||" \
		| paste - - \
		| grep '\.c' \
		| grep -v '/' \
		| sed 's/Lines executed://' \
		| awk '{ \
			match($$0,/[0-9]+\.[0-9]+%/); pct=substr($$0,RSTART,RLENGTH-1)+0; \
			match($$0,/of +([0-9]+)/); n=substr($$0,RSTART,RLENGTH); sub(/of +/,"",n); n=n+0; \
			hit+=pct*n/100; total+=n; \
			printf "  %-45s%7s\n", $$1, pct"%"; \
		} END { \
			printf "  %-45s%7s\n", "---", "--"; \
			if(total>0) printf "  %-45s%6.2f%%\n", "total", hit/total*100; \
		}'

clean:
	rm -rf $(BUILD_DIR) $(ROOT_DIR)/bin

# Package: copy libraries and headers into bin/ for distribution
LIB_DIR = $(ROOT_DIR)/bin

PUBLIC_HEADERS = audio_metadata.h audio_stream.h

lib: $(STATIC_LIB) $(SHARED_LIB)
	@mkdir -p $(LIB_DIR)/include
	@cp $(STATIC_LIB) $(LIB_DIR)/
	@cp $(SHARED_LIB) $(LIB_DIR)/
	@$(foreach h,$(PUBLIC_HEADERS),cp $(SRC_DIR)/$(h) $(LIB_DIR)/include/;)
	@echo "Packaged into $(LIB_DIR)/"
	@echo "  libaudiotag.a"
	@echo "  libaudiotag.so"
	@echo "  include/"
	@ls $(LIB_DIR)/include/ | sed 's/^/    /'

.PHONY: all clean test coverage lib
.SILENT:
