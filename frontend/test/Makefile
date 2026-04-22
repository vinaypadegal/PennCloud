TARGETS = echo-test smtp-test pop3-test

all: $(TARGETS)

%.o: %.cc
	g++ -Iinclude $^ -c -o $@

http_server_test: http_server_test.o common.o
	g++ $^ -o $@

clean::
	rm -fv $(TARGETS) *.o *~
