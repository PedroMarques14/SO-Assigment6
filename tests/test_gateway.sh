#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_gateway_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_gateway.sh"
        test_result=$?
        rm -rf "$temporary_root"
        exit "$test_result"
    fi
    rm -f "$fifo_probe"
fi

manager_pid=""
cleanup() {
    if [[ -n "$manager_pid" ]] && kill -0 "$manager_pid" 2>/dev/null; then
        kill -TERM "$manager_pid" 2>/dev/null || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -f SENSOR_PIPE COMMAND_PIPE
}
trap cleanup EXIT

rm -f SENSOR_PIPE COMMAND_PIPE logs/home_iot_log.txt
./home_iot &
manager_pid=$!

for attempt in {1..20}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then
        break
    fi
    sleep 0.1
done

if [[ ! -p SENSOR_PIPE || ! -p COMMAND_PIPE ]]; then
    echo "FIFOs were not created" >&2
    exit 1
fi

printf 'SALA_T1#TEMP_SALA#22\nlinha_invalida\n' > SENSOR_PIPE
kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""

if [[ -e SENSOR_PIPE || -e COMMAND_PIPE ]]; then
    echo "FIFOs were not removed after telemetry" >&2
    exit 1
fi

if ! grep -q '\[SYSTEM\] \[INFO\] .*STOP' logs/home_iot_log.txt; then
    echo "Shutdown event was not logged after telemetry" >&2
    exit 1
fi

echo "gateway smoke test: OK"
