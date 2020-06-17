all: client test

CXXFLAGS = -Wall -Werror -pedantic

client: *.cpp *.hpp
	g++ $(CXXFLAGS) client.cpp -o client

test: *.cpp *.hpp
	g++ $(CXXFLAGS) test.cpp -o test

check: test
	./test

memcheck: test
	valgrind ./test

perf: client
	time ./client 42

perf2: client
	time ./client 43

dump_state:
	@# show only non-empty lines
	for f in *.state; do echo "------ $$f ------"; \
	  xxd $$f | grep -v '0000 0000 0000 0000 0000 0000 0000 0000'; \
	done

.PHONY: clean
clean:
	rm -f client test *.state

