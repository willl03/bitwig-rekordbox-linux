CC ?= gcc
CFLAGS ?= -fPIC -O2 -Wall -fvisibility=default
JAVA_HOME ?= $(shell dirname $$(dirname $$(readlink -f $$(which javac 2>/dev/null || which java))))
INCLUDES = -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux -I/usr/include/sqlcipher
LIBS = -lsqlcipher

TARGET = build/librekordbox_shim.so
SRC = src/rekordbox_shim.c

all: $(TARGET)

$(TARGET): $(SRC)
	@mkdir -p build
	$(CC) $(CFLAGS) -shared -o $@ $(SRC) $(INCLUDES) $(LIBS)

clean:
	rm -rf build

.PHONY: all clean