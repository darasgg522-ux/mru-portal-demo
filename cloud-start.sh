#!/bin/sh
# Render assigns PORT automatically. MRU_DATA_DIR selects SQLite/upload storage.
set -eu
exec /app/mru_server /app "${PORT:-10000}"
