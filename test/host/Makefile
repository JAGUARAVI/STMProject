# Host build of the ESWCap measurement maths.
#
# The firmware's maths layer is deliberately HAL-free, so the exact source files
# that run on the target can be compiled and exercised here. Only the flash
# store is stubbed.
#
#   make          build and run
#   make clean

CC      ?= cc
CSTD    := -std=c99
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
OPT     := -O2
INC     := -I../../include -I.

SRC := \
	../../src/measure/rc_fit.c \
	../../src/measure/osc_calc.c \
	../../src/measure/board.c \
	../../src/measure/fusion.c \
	../../src/measure/adc_measure.c \
	../../src/measure/osc_measure.c \
	../../src/measure/rmode_math.c \
	../../src/calib/calib.c \
	stubs.c \
	test_main.c

BIN := esc_test

.PHONY: all run clean

all: run

$(BIN): $(SRC)
	$(CC) $(CSTD) $(WARN) $(OPT) -DESWCAP_HOST_TEST $(INC) $(SRC) -lm -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
