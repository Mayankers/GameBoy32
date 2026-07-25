CC = gcc
CFLAGS = -Wall -Wextra
SRCS = main.c cpu.c mmu.c
gb: $(SRCS)
	$(CC) $(CFLAGS) -o gb $(SRCS)
clean:
	rm -f gb