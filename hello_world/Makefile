#Notes:
#1. The make target depends on how eRPC was compiled:
#   * If DTRANSPORT=dpdk, use `make dpdk`
#   * If DTRANSPORT=raw, use `make raw`
#   * If DTRANSPORT=infiniband, use `make infiniband`
LIBS = -lerpc -lpthread -lnuma -ldl
PKG_CONFIG_PATH=$(RTE_SDK)/build/install/lib/x86_64-linux-gnu/pkgconfig
LDFLAGS_SHARED = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs libdpdk)
LDFLAGS_STATIC = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --static --libs libdpdk)

error:
	@echo "Please choose one of the following targets: infiniband, raw, dpdk, clean"
	@exit 2
infiniband:
	g++ -g -std=c++11 -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
	g++ -g -std=c++11 -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_INFINIBAND=true
raw:
	g++ -g -std=c++11 -o server server.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
	g++ -g -std=c++11 -o client client.cc -I ../src -I ../third_party/asio/include -L ../build $(LIBS) -libverbs -DERPC_RAW=true
dpdk:
	g++ -g -std=c++11 -o server server.cc \
		-Wno-deprecated-declarations \
		-DERPC_DPDK=true -march=native \
		-I ../src -I ../third_party/asio/include -I $(RTE_SDK)/build/install/include \
		-L ../build $(LIBS) \
		-Wl,--no-whole-archive \
		-lnuma -ldl -libverbs -lmlx5 \
		$(LDFLAGS_STATIC)

	g++ -g -std=c++11 -o client client.cc \
		-Wno-deprecated-declarations \
		-DERPC_DPDK=true -march=native \
		-I ../src -I ../third_party/asio/include -I $(RTE_SDK)/build/install/include \
		-L ../build $(LIBS) \
		-Wl,--no-whole-archive \
		-lnuma -ldl -libverbs -lmlx5 \
		$(LDFLAGS_STATIC)
clean:
	rm server client

