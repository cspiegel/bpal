all: bpal

PKG=		Qt6Gui Qt6Core
CXXFLAGS=	-g -O -Wall -std=c++23 $(shell pkg-config $(PKG) --cflags)
LIBS=		$(shell pkg-config $(PKG) --libs)

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
