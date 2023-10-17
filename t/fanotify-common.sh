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

# Create an overlayfs mount
mount_overlayfs() {
    base_dir=$1
    work_dir=${base_dir}_work
    upper_dir=${base_dir}_upper
    overlay_dir=${base_dir}_overlay

    mkdir -p $base_dir $work_dir $upper_dir $overlay_dir && \
        mount -t overlay overlay \
        -o lowerdir=$base_dir,upperdir=$upper_dir,workdir=$work_dir \
        $overlay_dir
}

# Test if we're running as root
is_root() {
    [ $(id -u) -eq 0 ]
}

# Test if overlayfs is supported
overlayfs_supported() {
    grep -q overlay /proc/filesystems 2>/dev/null
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
