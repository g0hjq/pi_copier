# Compiler to use
CC = gcc

# Compiler flags
CFLAGS = -Wall -g -ggdb -Werror -O2 -D_POSIX_C_SOURCE=200112L -pthread -DINI_MAX_LINE=512 

# Linker flags
LDFLAGS = -ludev -lrt -lgpiod

# Target executables
SERVER = server
CLIENT = client

# Source files
SERVER_SRC = server.c lcd.c utilities.c gpio.c usb.c
CLIENT_SRC = client.c utilities.c usb.c

# Header files
HEADERS = globals.h lcd.h utilities.h gpio.h usb.h

# Object files
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)

# Default target
all: $(SERVER) $(CLIENT)

# Link server executable
$(SERVER): $(SERVER_OBJ)
	$(CC) $(SERVER_OBJ) -o $(SERVER) $(LDFLAGS)

# Link client executable
$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CLIENT_OBJ) -o $(CLIENT) $(LDFLAGS)


# Compile server.c to server.o
server.o: server.c $(HEADERS)
	$(CC) $(CFLAGS) -c server.c -o server.o

# Compile client.c to client.o
client.o: client.c $(HEADERS)
	$(CC) $(CFLAGS) -c client.c -o client.o

# Compile lcd.c to lcd.o
lcd.o: lcd.c $(HEADERS)
	$(CC) $(CFLAGS) -c lcd.c -o lcd.o

# Compile gpio.c to gpio.o
gpio.o: gpio.c $(HEADERS)
	$(CC) $(CFLAGS) -c gpio.c -o gpio.o

# Compile usb.c to usb.o
usb.o: usb.c $(HEADERS)
	$(CC) $(CFLAGS) -c usb.c -o usb.o

# Compile utilities.c to utilities.o
utilities.o: utilities.c $(HEADERS)
	$(CC) $(CFLAGS) -c utilities.c -o utilities.o

# Clean up
clean:
	rm -f $(SERVER) $(CLIENT) $(SETUP) $(SERVER_OBJ) $(CLIENT_OBJ)$(SETUP_OBJ)

# Phony targets
.PHONY: all clean
