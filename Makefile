C_SRCS = test.c smoldtb.c
C_FLAGS = -O0 -Wall -Wextra -g -DSMOLDTB_STATIC_BUFFER_SIZE=0x4000
TARGET = test.elf

all: $(C_SRCS)
	gcc $(C_SRCS) $(C_FLAGS) -o $(TARGET)

run: all
	./$(TARGET)

debug: all
	gdb ./$(TARGET)

clean:
	rm test.elf

