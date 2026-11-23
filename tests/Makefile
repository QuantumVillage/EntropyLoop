CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -D_DEFAULT_SOURCE
CPPFLAGS += -I../src
LDLIBS += -lm

all: test replay

test_health: test_health.c ../src/health_tests.c ../src/health_tests.h ../src/health_cutoffs.h sim_source.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ test_health.c ../src/health_tests.c $(LDLIBS)

simulate: simulate.c ../src/health_tests.c ../src/health_tests.h ../src/health_cutoffs.h sim_source.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ simulate.c ../src/health_tests.c $(LDLIBS)

replay: replay.c ../src/health_tests.c ../src/health_tests.h ../src/health_cutoffs.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ replay.c ../src/health_tests.c $(LDLIBS)

test: test_health
	./test_health

demo: simulate
	./simulate

clean:
	rm -f test_health simulate replay

.PHONY: all test demo clean
