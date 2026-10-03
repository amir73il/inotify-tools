#!/bin/sh

# Check for kernel support and privileges on a path.
# Extra arguments are fsnotifywait options, such as --filesystem.
fanotify_supported_on() {
    path=$1
    shift
    # "Negative timeout" means the watch works.
    # "Operation not permitted" means fanotify is supported, but this user cannot use it.
    ../../src/fsnotifywait --fanotify -t -1 "$@" "$path" 2>&1 |
        grep -q -E 'Negative timeout|Operation not permitted'
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

# Create and mount a btrfs filesystem with subvolumes
mount_btrfs_with_subvolumes() {
    size=$1
    mnt=$2
    subvol_name=${3:-subvol1}

    mount_filesystem btrfs $size $mnt && \
        btrfs subvolume create $mnt/$subvol_name
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

# Test if we can create and mount a btrfs filesystem
btrfs_supported() {
    which mkfs.btrfs >/dev/null 2>&1 && which btrfs >/dev/null 2>&1 || return 1
    img=$(mktemp) || return 1
    mnt=$(mktemp -d) || { rm -f "$img"; return 1; }
    truncate -s 120M "$img" && \
        mkfs.btrfs "$img" >/dev/null 2>&1 && \
        mount -o loop "$img" "$mnt" >/dev/null 2>&1
    rc=$?
    umount "$mnt" >/dev/null 2>&1
    rm -rf "$mnt" "$img"
    return $rc
}

# Test if overlayfs is supported
overlayfs_supported() {
    grep -q overlay /proc/filesystems 2>/dev/null
}

# Run a binary chrooted to $1 in a private mount namespace
run_in_chroot() {
    root=$1
    shift
    unshare -m sh -c '
        root=$1
        bind_file() {
            mkdir -p "$root$(dirname "$1")" &&
                touch "$root$1" &&
                mount --bind "$1" "$root$1"
        }
        mount --make-rprivate / &&
            mkdir -p "$root/proc" &&
            mount -t proc proc "$root/proc" &&
            bind_file "$2" &&
            for lib in $(ldd "$2" | grep -o "/[^ ]*"); do
                bind_file "$lib" || exit 1
            done &&
            shift &&
            exec chroot "$root" "$@"
    ' sh "$root" "$@"
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
