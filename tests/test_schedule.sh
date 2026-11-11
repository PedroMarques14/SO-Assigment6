#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_schedule_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_schedule.sh"
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

rm -f SENSOR_PIPE COMMAND_PIPE
./home_iot &
manager_pid=$!
for attempt in {1..20}; do
    if [[ -p SENSOR_PIPE && -p COMMAND_PIPE ]]; then break; fi
    sleep 0.1
done

scheduled=$(printf 'schedule S1 at: 5 actuator: ILUMINACAO duration: 20\nstatus\nexit\n' | ./user_console 1)
if ! grep -q 'OK schedule accepted' <<< "$scheduled" || ! grep -q 'ILUMINACAO status=0' <<< "$scheduled"; then
    printf '%s\n' "$scheduled" >&2
    echo "Schedule was not accepted or activated too early" >&2
    exit 1
fi
invalid=$(printf 'schedule S2 at: 0 actuator: ILUMINACAO duration: 5\nexit\n' | ./user_console 1)
if ! grep -q 'ERROR invalid schedule instant' <<< "$invalid"; then
    echo "Past schedule was not rejected" >&2
    exit 1
fi
sleep 1.2
active=$(printf 'status\nexit\n' | ./user_console 1)
if ! grep -q 'ILUMINACAO status=1' <<< "$active"; then
    printf '%s\n' "$active" >&2
    echo "Schedule did not activate at the expected time" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""
echo "schedule smoke test: OK"
