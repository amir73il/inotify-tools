#!/bin/bash

# VM Test Script for fsnotifywait --filesystem mode
# Run this script as ROOT

echo "=== VM FSNOTIFYWAIT FILESYSTEM TEST ==="

# Update fsnotifywait binary if available
if [ -f /vtmp/inotifywait ]; then
    echo "Updating fsnotifywait binary from /vtmp..."
    cp /vtmp/inotifywait /usr/sbin/fsnotifywait
    chmod +x /usr/sbin/fsnotifywait
    echo "fsnotifywait updated to /usr/sbin/"
fi

# Create and mount tmpfs for testing
echo ""
echo "=== SETTING UP TMPFS ==="
mkdir -p /tmp/fstest
mount -t tmpfs tmpfs /tmp/fstest
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to mount tmpfs!"
    exit 1
fi

echo "tmpfs mounted at /tmp/fstest"
df -h /tmp/fstest

# Set ownership so testuser can create files
echo "Setting tmpfs ownership to testuser for user access..."
chown testuser:testuser /tmp/fstest

# Start fsnotifywait in background
echo ""
echo "=== STARTING FSNOTIFYWAIT --FILESYSTEM ==="
echo "fsnotifywait --filesystem /tmp/fstest -m"
cd /tmp/fstest
/usr/sbin/fsnotifywait --filesystem /tmp/fstest -m &
FSNOTIFY_PID=$!
sleep 2

# Check if fsnotifywait is running
if ! kill -0 $FSNOTIFY_PID 2>/dev/null; then
    echo "ERROR: fsnotifywait failed to start!"
    umount /tmp/fstest
    exit 1
fi

echo "fsnotifywait started with PID: $FSNOTIFY_PID"

# Generate filesystem events
echo ""
echo "=== GENERATING FILESYSTEM EVENTS ==="

echo "1. Creating simple files..."
touch fsnotify_file1.txt
echo "content from fsnotifywait" > fsnotify_file1.txt

echo "2. Creating directories and files..."
mkdir fsnotify_dir
touch fsnotify_dir/file_in_dir.txt
echo "nested content" > fsnotify_dir/file_in_dir.txt

echo "3. Creating nested directory structure..."
mkdir -p fsnotify_nested/a/b/c/d
touch fsnotify_nested/a/root_file.txt
touch fsnotify_nested/a/b/level2_file.txt
touch fsnotify_nested/a/b/c/level3_file.txt
touch fsnotify_nested/a/b/c/d/deep_file.txt

echo "4. Modifying files..."
echo "modified content" >> fsnotify_file1.txt
echo "more nested content" >> fsnotify_dir/file_in_dir.txt
echo "deep modification" >> fsnotify_nested/a/b/c/d/deep_file.txt

echo "5. Moving/renaming files..."
mv fsnotify_file1.txt fsnotify_file1_renamed.txt
mv fsnotify_dir/file_in_dir.txt fsnotify_dir/moved_file.txt

echo "6. Deleting files and directories..."
rm fsnotify_file1_renamed.txt
rm -rf fsnotify_dir
rm -rf fsnotify_nested

echo ""
echo "=== EVENTS GENERATED ==="
echo "All test operations completed."
echo "Let fsnotifywait capture events for a few more seconds..."
sleep 3

# Stop fsnotifywait
echo ""
echo "=== STOPPING FSNOTIFYWAIT ==="
echo "Terminating fsnotifywait process..."
kill $FSNOTIFY_PID 2>/dev/null
wait $FSNOTIFY_PID 2>/dev/null
echo "fsnotifywait stopped."

# Verify cleanup
echo ""
echo "=== VERIFYING CLEANUP ==="
echo "Remaining files in /tmp/fstest created by fsnotifywait test:"
ls -la /tmp/fstest | grep fsnotify || echo "No fsnotify files remaining (good!)"

# Cleanup
echo ""
echo "=== CLEANUP ==="
cd /tmp
umount /tmp/fstest
rmdir /tmp/fstest
echo "Cleanup complete."

echo ""
echo "=== TEST COMPLETE ==="
echo "Check the output above for filesystem events captured by fsnotifywait."
echo "You should see CREATE, MODIFY, DELETE events with full path resolution."
