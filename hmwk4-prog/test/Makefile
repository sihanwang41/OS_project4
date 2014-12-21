INCLUDE = -I. -I./inc -I../flo-kernel/include -I../flo-kernel/arch/arm/include
CC := arm-none-linux-gnueabi-gcc
LD := arm-none-linux-gnueabi-gcc
CFLAGS := -g -Wall -Werror -static $(INCLUDE)
LDFLAGS := -static

OBJECTS := test.o

all: test 


: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS)

%.o: %.c 
	$(CC) $(CFLAGS) -c $^

clean:
	rm -f test 
	rm -f test.o

.PHONY: clean
