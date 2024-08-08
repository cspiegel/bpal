all: bpal

CXXFLAGS=	-g -O -Wall -std=c++23
LIBS=		$(shell pkg-config Qt6Gui Qt6Core --cflags --libs)

ifndef NO_LIBOXI
    CXXFLAGS+=	-DLIBOXI -Ioxi
    LIBS+=	oxi/target/release/liboxi.a
    OXI_BUILD=	cd oxi && cargo build --release
endif

bpal: bpal.cpp
	$(OXI_BUILD)
	$(CXX) $(CXXFLAGS) $^ -o bpal $(LIBS)
clean:
	rm -f bpal

.PHONY: all clean
