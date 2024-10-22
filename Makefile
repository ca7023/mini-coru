OUTPUT := out
TEST_SUBDIR := test
SRCS := $(wildcard *.c)
TEST_SRCS := $(wildcard test/*.c)
OBJS := $(SRCS:%.c=${OUTPUT}/%.o)
TEST_EXES := $(TEST_SRCS:%.c=%)
CFLAGS ?=
CFLAGS += -g -Wall -lrt -O0
SONAME := libminicoru.so

all: test

outputdir:
	mkdir -p $(OUTPUT)/$(TEST_SUBDIR)

build: ${OBJS} | outputdir
	gcc $(CFLAGS) -shared -o $(OUTPUT)/$(SONAME) $^

test: build $(TEST_EXES)

$(TEST_EXES): $(TEST_SRCS) | outputdir
	gcc $(CFLAGS) -o $(OUTPUT)/$@ $(OUTPUT)/$(SONAME) $@.c

$(OBJS): $(SRCS) | outputdir
	gcc $(CFLAGS) -o $@ -fPIC -c $<

clean:
	rm -rf ${OUTPUT}

