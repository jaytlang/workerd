#!/bin/sh
# yay leak detection

set +x

preload="scripts/mleakdetect/mleakdetect.so"
doas env LD_PRELOAD=$preload workerd -dv 2> log.txt
