CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lX11 -lXrender -lcairo -lavcodec -lavformat -lavutil -lswscale -lXrandr

TARGET = aniwall
SRCS = aniwall.c
OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) 