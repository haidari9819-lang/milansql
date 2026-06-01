FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    cmake g++ ninja-build curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /milansql
COPY . .

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 4

EXPOSE 4406 4407 5433 8080 8081

ENV MILANSQL_PORT=4406
ENV MILANSQL_MYSQL_PORT=4407
ENV MILANSQL_PG_PORT=5433
ENV MILANSQL_HTTP_PORT=8080
ENV MILANSQL_GRAPHQL_PORT=8081

COPY docker-entrypoint.sh /docker-entrypoint.sh
RUN chmod +x /docker-entrypoint.sh

ENTRYPOINT ["/docker-entrypoint.sh"]
