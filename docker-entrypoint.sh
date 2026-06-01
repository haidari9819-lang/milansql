#!/bin/bash
set -e
echo "Starting MilanSQL v4.0.0..."
echo "  TCP:      port ${MILANSQL_PORT:-4406}"
echo "  MySQL:    port ${MILANSQL_MYSQL_PORT:-4407}"
echo "  Postgres: port ${MILANSQL_PG_PORT:-5433}"
echo "  HTTP:     port ${MILANSQL_HTTP_PORT:-8080}"
echo "  GraphQL:  port ${MILANSQL_GRAPHQL_PORT:-8081}"

exec ./build/milansql \
  --server --port ${MILANSQL_PORT:-4406} \
  --mysql --mysql-port ${MILANSQL_MYSQL_PORT:-4407} \
  --pg --pg-port ${MILANSQL_PG_PORT:-5433} \
  --http --http-port ${MILANSQL_HTTP_PORT:-8080} \
  --graphql --graphql-port ${MILANSQL_GRAPHQL_PORT:-8081}
