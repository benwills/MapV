SRCS   := MapV.c
OBJS   := mapv.o
CFLAGS := -O3 -lm -Wall -march=native -mavx -mavx2 -march=native -lxxhash

# ALL TARGET

.PHONY: all clean
all: mapv.o MapV_test

mapv.o: MapV.c
	$(CC) -c -o $@ $^ $(CFLAGS)

MapV_test.o: MapV_test.c
	$(CC) -c -o $@ $^ $(CFLAGS)

MapV_test: MapV_test.o mapv.o
	$(CC) -o $@ MapV_test.o mapv.o $(CFLAGS)

clean:
	rm -rf *.o
	rm MapV_test
