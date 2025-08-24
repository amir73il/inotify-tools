#!/bin/bash

# VM Server Test Script for fsnotifywait server functionality
# Run this script as ROOT

echo "=== VM FSNOTIFYWAIT SERVER TEST (ROOT) ==="

echo "Installing fsnotifywait from /vtmp to /usr/sbin/..."
if [ -f /vtmp/inotifywait ]; then
    cp /vtmp/inotifywait /usr/sbin/fsnotifywait
    chmod +x /usr/sbin/fsnotifywait
    echo "fsnotifywait copied and installed to /usr/sbin/"

fi

# Create and mount tmpfs for testing
echo ""
echo "=== SETTING UP TMPFS ==="
mkdir -p /tmp/vmtest/secrets
chmod 777 /tmp/vmtest/secrets
mount -t tmpfs tmpfs /tmp/vmtest
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to mount tmpfs!"
    exit 1
fi

echo "tmpfs mounted at /tmp/vmtest"
df -h /tmp/vmtest

# Set ownership so testuser can create files
echo "Setting tmpfs ownership to testuser for testuser access..."
chown testuser:testuser /tmp/vmtest

# Start server in filesystem mode
echo ""
echo "=== STARTING FSNOTIFYWAIT SERVER ==="
echo "fsnotifywait --server /tmp/vmsock --filesystem /tmp/vmtest"
/usr/sbin/fsnotifywait --server /tmp/vmsock --filesystem /tmp/vmtest &
SERVER_PID=$!
sleep 2

# Check if socket was created
echo "Checking server socket..."
ls -la /tmp/vmsock
if [ ! -S /tmp/vmsock ]; then
    echo "ERROR: Server socket not created!"
    kill $SERVER_PID 2>/dev/null
    umount /tmp/vmtest
    exit 1
fi

# Set socket permissions for testuser access
echo "Setting socket permissions to 666 for testuser access..."
chmod 666 /tmp/vmsock
ls -la /tmp/vmsock

echo ""
echo "=== SERVER SETUP COMPLETE ==="
echo "Server is running with PID: $SERVER_PID"
echo "Socket: /tmp/vmsock (permissions: 666)"
echo "Mount: /tmp/vmtest (permissions: 777)"
echo ""
echo "Now run the client script as testuser:"
echo "  su - testuser -c '/vtmp/vm_client_test.sh'"
echo ""
echo "Press Ctrl+C to stop server and cleanup..."

# Wait for interrupt
trap 'echo ""; echo "=== CLEANING UP SERVER ==="; kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null; rm -f /tmp/vmsock; cd /tmp; umount /tmp/vmtest 2>/dev/null; rmdir /tmp/vmtest 2>/dev/null; echo "Server cleanup complete."; exit 0' INT

# Keep server running
wait $SERVER_PID
echo "Server process ended."
