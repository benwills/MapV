CC     := gcc
SRCS   := MapV.c
OBJS   := MapV.o
CFLAGS := -O3 -lm -Wall -mavx -mavx2 -march=native -lxxhash -I/usr/local/include -L/usr/local/lib -lxxhash

# ALL TARGET

.PHONY: all clean test
all: MapV_test MapV_testObjArr

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

MapV_test: MapV_test.o
	$(CC) -o $@ MapV_test.o $(CFLAGS)

MapV_testObjArr: MapV_testObjArr.o
	$(CC) -o $@ MapV_testObjArr.o $(CFLAGS)

test:
	./MapV_test ./input.stop_words.536.txt
	./MapV_test ./input.ips_sort_of.3901.txt
	./MapV_test ./input.english_words.10k.txt
	./MapV_test ./input.alexa_domains.1M.txt
	./MapV_testObjArr

clean:
	rm -rf *.o
	rm MapV_test       || true
	rm MapV_testObjArr || true
