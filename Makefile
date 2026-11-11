CC = gcc
CFLAGS = -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread -g -O2 -Iinclude
LDFLAGS = -pthread -lrt

TARGETS = home_iot sensor user_console
COMMON_OBJS = src/sync_utils.o src/log_manager.o
SERVER_OBJS = src/main.o src/ipc_utils.o src/config_parser.o src/command_dispatcher.o src/sensor_gateway.o src/actuator_ctrl.o src/alerts_watcher.o src/historian.o $(COMMON_OBJS)

.PHONY: all clean test directories

all: directories $(TARGETS)

directories:
	mkdir -p logs history results

home_iot: $(SERVER_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

sensor: src/sensor.o
	$(CC) -o $@ $^ $(LDFLAGS)

user_console: src/user_console.o
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGETS)
	rm -f SENSOR_PIPE COMMAND_PIPE
	rm -f logs/*.txt history/*.csv results/*.txt

test: all
	bash tests/test_basic.sh
	bash tests/test_gateway.sh
	bash tests/test_console.sh
	bash tests/test_actuators.sh
	bash tests/test_alerts.sh
	bash tests/test_historian.sh
	bash tests/test_admin.sh
	bash tests/test_schedule.sh
	bash tests/test_stress.sh
	bash tests/test_signals.sh
