
CFLAGSW_GCC = -Wall -Wextra -Wno-missing-field-initializers \
    -Wno-parentheses -Wno-missing-braces \
    -Wmissing-prototypes -Wfloat-equal \
    -Wwrite-strings -Wpointer-arith -Wcast-align \
    -Wnull-dereference \
    -Werror=multichar -Werror=sizeof-pointer-memaccess -Werror=return-type \
    -fstrict-aliasing

CFLAGS0 = -pthread -g
CFLAGS1 = -O3

CFLAGS = $(CFLAGSW_GCC) $(CFLAGS0) $(CFLAGS1)

.PHONY: all
all: osjitter pingpong

osjitter: util.o

pingpong: util.o

ptp-clock-offset: util.o

.PHONY: clean
clean:
	rm -f osjitter osjitter.o util.o pingpong pingpong.o ptp-clock-offset
