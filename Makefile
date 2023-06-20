C_SRCS = test.c smoldtb.c
C_FLAGS = -O0 -Wall -Wextra -g
TARGET = test.elf

all: $(C_SRCS)
	gcc $(C_SRCS) $(C_FLAGS) -o $(TARGET)

run: all
	./$(TARGET)

debug: all
	gdb ./$(TARGET)

clean:
	rm test.elf

