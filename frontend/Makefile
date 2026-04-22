TARGETS = chatserver chatclient

all: $(TARGETS)

%.o: %.cc
	g++ $^ -c -o $@

http_server: http_server.cc
	g++ $< -lpthread -g -o $@
	
http_server_test: http_server_test.cc
	g++ $< -lpthread -g -o $@

pack:
	rm -f project.zip
	zip -r project.zip README Makefile *.c* *.h*

clean::
	rm -fv $(TARGETS) *~ *.o project.zip

realclean:: clean
	rm -fv project.zip
