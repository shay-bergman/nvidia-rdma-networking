CC      = gcc
CFLAGS  = -g -Wall -O2
LDFLAGS = -libverbs -lrt

# Library modules implementing the collective, plus the test driver.
LIB_SRC  = connection.c memory.c transport.c collective.c
TEST_SRC = test.c

LIB_OBJ  = $(LIB_SRC:.c=.o)
TEST_OBJ = $(TEST_SRC:.c=.o)
TARGET   = test_rdma

all: $(TARGET)

$(TARGET): $(LIB_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(LIB_OBJ) $(TEST_OBJ) $(LDFLAGS)

# Library modules depend on both internal and public headers.
$(LIB_OBJ): %.o: %.c pg_internal.h rdma_lib.h
	$(CC) $(CFLAGS) -c $< -o $@

# The test driver only needs the public header.
$(TEST_OBJ): %.o: %.c rdma_lib.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(LIB_OBJ) $(TEST_OBJ)

.PHONY: all clean
