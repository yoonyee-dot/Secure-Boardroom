CC = gcc
CFLAGS = -Wall -Wextra -pthread -Wno-unused-parameter

TARGET_SERVER = server
TARGET_CLIENT = client
TARGET_CLIENT2 = client2

SRC_SERVER = server.c
SRC_CLIENT = client.c
SRC_CLIENT2 = client2.c

all: $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_CLIENT2)

$(TARGET_SERVER): $(SRC_SERVER)
   $(CC) $(CFLAGS) -o $(TARGET_SERVER) $(SRC_SERVER)

$(TARGET_CLIENT): $(SRC_CLIENT)
   $(CC) $(CFLAGS) -o $(TARGET_CLIENT) $(SRC_CLIENT)

$(TARGET_CLIENT2): $(SRC_CLIENT2)
   $(CC) $(CFLAGS) -o $(TARGET_CLIENT2) $(SRC_CLIENT2)

clean:
   rm -f $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_CLIENT2)

re:
   make clean
   make
