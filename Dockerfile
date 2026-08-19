FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make pkg-config \
        libevent-dev libjsoncpp-dev libhiredis-dev libssl-dev \
        libmysqlclient-dev ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j \
    && mkdir -p /var/cloudstore/data /var/log/cloudstore \
    && cp build/cloud_server build/cloud_client /usr/local/bin/ \
    && cp conf/server.conf /etc/cloudstore.conf

EXPOSE 9000
CMD ["cloud_server", "-c", "/etc/cloudstore.conf"]
