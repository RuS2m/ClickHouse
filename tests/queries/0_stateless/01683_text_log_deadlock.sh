#!/usr/bin/env bash
# Tags: deadlock, no-parallel

CURDIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../shell_config.sh
. "$CURDIR"/../shell_config.sh

$CLICKHOUSE_BENCHMARK -i 5000 -c 32 --query 'SELECT 1' |& grep -oF 'queries: 5000'
