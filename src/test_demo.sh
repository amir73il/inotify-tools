#!/bin/sh -x

WD=${1:-"."}
cd $WD

#echo "file fs/notify/fanotify/*  +p" > /sys/kernel/debug/dynamic_debug/control
rm -rf a
mkdir -p a/b/c/d/e/f/g/

# Sleep 2 seconds while generating events and then process events
inotifywatch --global --writes --timeout -2 $WD &

sleep 1
#SLEEP='sleep 1'
$SLEEP
chmod +x a
touch a/0 a/1 a/2 a/3
mkdir a/dir0 a/dir1 a/dir2
$SLEEP
mv a/0 a/3
mv a/dir0 a/dir3
$SLEEP
rm a/1
rmdir a/dir1
$SLEEP
chmod +x a/b/c/d
touch a/b/c/d/e/f/g/0
$SLEEP
mv a/b/c/d/e/f/g/0 a/b/c/d/e/f/1
$SLEEP
mv a/b/c/d/e/f/g a/b/c/d/e/G
