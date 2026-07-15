CC = gcc
CFLAGS = -O2 -Wall
LIBS = -lcurl -lpthread

tsharp: main.c lexer.c tparser.c tinterp.c lexer.h tparser.h tinterp.h
	$(CC) $(CFLAGS) main.c lexer.c tparser.c tinterp.c -o tsharp $(LIBS)

test: tsharp
	./tsharp examples/test_basics.tsharp
	./tsharp examples/test_orchestra.tsharp
	./tsharp examples/test_parallel.tsharp

clean:
	rm -f tsharp tsharp.exe ai_debug.log out_*.txt *.bak

.PHONY: test clean
