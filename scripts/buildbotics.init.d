#!/bin/bash
#
#   Buildbotics Web server
#
#  Add to system with:
#    sudo update-rc.d buildbotics defaults 90

### BEGIN INIT INFO
# Provides: buildbotics
# Required-Start:
# Required-Stop:
# Should-Start:
# Should-Stop:
# Default-Start: 2 3 4 5
# Default-Stop: 0 1 6
# Short-Description: Start and stop Buildbotics Web server
# Description: Buildbotics Web server
### END INIT INFO

NAME=buildbotics
EXEC=/usr/bin/$NAME
CONFIG=/etc/$NAME/config.xml
USER=$NAME
RUN=/var/run/$NAME
PID_FILE=$RUN/$NAME.pid

START_STOP_OPTS="-x $EXEC -n $NAME -p $PID_FILE"


start() {
    mkdir -p $(dirname $PID_FILE)
    start-stop-daemon --start $START_STOP_OPTS -- \
      --config $CONFIG --daemon --run-as $USER --pid-file $PID_FILE \
      --chdir $RUN
}


stop() {
    start-stop-daemon --stop --retry forever/-TERM/10 $START_STOP_OPTS
}


status() {
    start-stop-daemon --status $START_STOP_OPTS
}


################################################################################
# Main switch statement
#
case "$1" in
    start) start ;;
    stop) stop ;;
    restart) stop; start ;;
    status) status ;;

    *)
      echo "Syntax: $0 <start | stop | restart | status>"
      exit 1
    ;;
esac
