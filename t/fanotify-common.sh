#!/bin/sh

# Check for kernel support and privileges on a path.
# Extra arguments are fsnotifywait options, such as --filesystem.
fanotify_supported_on() {
    path=$1
    shift
    ../../src/fsnotifywait --fanotify -t -1 "$@" "$path" 2>&1 | grep -q 'Negative timeout'
}

fanotify_supported() {
    fanotify_supported_on "." "$@"
}

# Create and mount a test filesystem
mount_filesystem() {
    fstype=$1
    size=$2
    mnt=$3
    rm -f img
    truncate -s $size img && mkfs.$fstype img && \
        mkdir -p $mnt && mount -o loop img $mnt && \
        df -t $fstype $mnt
}

# Create tmpfs mount
mount_tmpfs() {
    mnt=$1
    size=${2:-10M}
    mkdir -p $mnt && mount -t tmpfs -o size=$size tmpfs $mnt
}

# Test if we're running as root
is_root() {
    [ $(id -u) -eq 0 ]
}

# Clean up filesystem mounts
cleanup_mounts() {
    for mnt in "$@"; do
        if mountpoint -q "$mnt" 2>/dev/null; then
            umount -l "$mnt" 2>/dev/null || true
        fi
        rm -rf "$mnt" 2>/dev/null || true
    done
}
