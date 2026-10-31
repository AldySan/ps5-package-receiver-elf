ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := pkg-receiver.elf
SRCS := main.c notify.c discovery.c installer.c http.c files.c \
        webui.c pull.c server.c

CFLAGS := -Os -Wall -Werror

ifdef TEST_ONLY
    CFLAGS += -DTEST_ONLY
    ELF := pkg-receiver-testonly.elf
endif

all: $(ELF)

$(ELF): $(SRCS) $(wildcard *.h)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lkernel_sys -lSceNet
	$(STRIP) --strip-all $@

clean:
	rm -f pkg-receiver.elf pkg-receiver-testonly.elf
