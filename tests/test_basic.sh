#!/usr/bin/env bash
set -u

if [[ "${HOME_IOT_TEST_IN_LINUX_FS:-0}" != "1" ]]; then
    fifo_probe="$PWD/.home_iot_fifo_probe.$$"
    if ! mkfifo "$fifo_probe" 2>/dev/null; then
        temporary_root=$(mktemp -d)
        cp -a . "$temporary_root/project"
        HOME_IOT_TEST_IN_LINUX_FS=1 bash -c "cd '$temporary_root/project' && bash tests/test_basic.sh"
        test_result=$?
        rm -rf "$temporary_root"
        exit "$test_result"
    fi
    rm -f "$fifo_probe"
fi

manager_pid=""
queue_count() {
    ipcs -q 2>/dev/null | awk 'NR > 3 && NF > 0 { count++ } END { print count + 0 }'
}
shared_memory_count() {
    ipcs -m 2>/dev/null | awk 'NR > 3 && NF > 0 { count++ } END { print count + 0 }'
}
cleanup() {
    if [[ -n "$manager_pid" ]] && kill -0 "$manager_pid" 2>/dev/null; then
        kill -TERM "$manager_pid" 2>/dev/null || true
        wait "$manager_pid" 2>/dev/null || true
    fi
    rm -f SENSOR_PIPE COMMAND_PIPE
}
trap cleanup EXIT

rm -f SENSOR_PIPE COMMAND_PIPE logs/home_iot_log.txt
queues_before=$(queue_count)
shared_memory_before=$(shared_memory_count)
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

queues_after=$(queue_count)
shared_memory_after=$(shared_memory_count)
if (( queues_after < queues_before + 3 )); then
    echo "System V message queues were not created" >&2
    exit 1
fi
if (( shared_memory_after < shared_memory_before + 4 )); then
    echo "System V shared memory segments were not created" >&2
    exit 1
fi
if [[ ! -e "/dev/shm/sem.sem_home_energy_$(id -u)" || ! -e "/dev/shm/sem.sem_home_log_$(id -u)" ]]; then
    echo "Named POSIX semaphores were not created" >&2
    exit 1
fi

kill -INT "$manager_pid"
wait "$manager_pid"
manager_pid=""

if [[ -e SENSOR_PIPE || -e COMMAND_PIPE ]]; then
    echo "FIFOs were not removed" >&2
    exit 1
fi

if (( $(queue_count) != queues_before )); then
    echo "System V message queues were not removed" >&2
    exit 1
fi
if (( $(shared_memory_count) != shared_memory_before )); then
    echo "System V shared memory segments were not removed" >&2
    exit 1
fi
if [[ -e "/dev/shm/sem.sem_home_energy_$(id -u)" || -e "/dev/shm/sem.sem_home_log_$(id -u)" ]]; then
    echo "Named POSIX semaphores were not removed" >&2
    exit 1
fi

if ! grep -q '\[SYSTEM\] \[INFO\] .*STOP' logs/home_iot_log.txt; then
    echo "Shutdown event was not logged" >&2
    exit 1
fi

echo "basic smoke test: OK"
