# Build the C++/Boost/SQLite backend in a Debian environment.
FROM debian:trixie-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ libboost-json-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY server.cpp .
RUN g++ -std=c++17 -O1 -pthread server.cpp -o /mru_server -lsqlite3

# Keep the published image small: only runtime libraries and website assets.
FROM debian:trixie-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 libstdc++6 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /mru_server ./mru_server
COPY index.html ./index.html
COPY cloud-start.sh ./cloud-start.sh
RUN chmod +x ./cloud-start.sh && mkdir -p /app/runtime-data
ENV MRU_HOST=0.0.0.0
ENV MRU_DATA_DIR=/app/runtime-data
EXPOSE 10000
CMD ["./cloud-start.sh"]
