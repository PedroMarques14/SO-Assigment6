#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_signals_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_signals.sh"
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

rm -f SENSOR_PIPE COMMAND_PIPE manager.out results/stats_snapshot_*.txt
./home_iot > manager.out 2>&1 &
manager_pid=$!
for attempt in {1..20}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then break; fi
    sleep 0.1
done

kill -USR1 "$manager_pid"
sleep 0.2
kill -USR2 "$manager_pid"
sleep 0.2
if ! grep -q 'Home IoT: readings=' manager.out; then
    echo "SIGUSR1 summary was not printed" >&2
    exit 1
fi
if ! compgen -G 'results/stats_snapshot_*.txt' > /dev/null; then
    echo "SIGUSR2 snapshot was not generated" >&2
    exit 1
fi
kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "signals smoke test: OK"
