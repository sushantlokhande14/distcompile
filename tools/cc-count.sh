#!/bin/sh
# gcc wrapper for tools/workload.py: logs every compile (-c) then runs gcc
case " $* " in *" -c "*) echo "$*" >> "${CC_LOG:-/tmp/cc-count.log}" ;; esac
exec gcc "$@"
