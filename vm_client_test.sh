#!/bin/bash

# VM Client Test Script for fsnotifywait client functionality
# Run this script as testuser (non-root)

echo "=== VM FSNOTIFYWAIT CLIENT TEST (TESTUSER) ==="

# Check if fsnotifywait is available
echo "Checking fsnotifywait availability..."
if [ ! -x /usr/sbin/fsnotifywait ]; then
    echo "ERROR: /usr/sbin/fsnotifywait not found!"
    exit 1
fi

echo "Running as user: $(whoami)"

# Check if server socket exists and is accessible
echo ""
echo "=== CHECKING SERVER SOCKET ==="
if [ ! -S /tmp/vmsock ]; then
    echo "ERROR: Server socket /tmp/vmsock not found!"
    echo "Make sure to run the server script as root first."
    exit 1
fi

ls -la /tmp/vmsock
echo "Socket found and accessible."

# Check if tmpfs mount is accessible
echo ""
echo "=== CHECKING TMPFS ACCESS ==="
if [ ! -d /tmp/vmtest ]; then
    echo "ERROR: tmpfs mount /tmp/vmtest not found!"
    exit 1
fi

ls -la /tmp/vmtest
echo "Testing write access..."
touch /tmp/vmtest/.test_write 2>/dev/null
if [ $? -eq 0 ]; then
    rm -f /tmp/vmtest/.test_write
    echo "Write access confirmed."
else
    echo "ERROR: No write access to /tmp/vmtest!"
    exit 1
fi

# Start client
echo ""
echo "=== STARTING FSNOTIFYWAIT CLIENT ==="
echo "fsnotifywait --client /tmp/vmsock -m /tmp/vmtest"
cd /tmp/vmtest
/usr/sbin/fsnotifywait --client /tmp/vmsock -m /tmp/vmtest &
CLIENT_PID=$!
sleep 2

# Check if client is running
if ! kill -0 $CLIENT_PID 2>/dev/null; then
    echo "ERROR: Client failed to start!"
    exit 1
fi

echo "Client started with PID: $CLIENT_PID"

# Generate filesystem events
echo ""
echo "=== GENERATING FILESYSTEM EVENTS ==="

echo "1. Creating simple files..."
touch testuser_file1.txt
echo "content from testuser" > testuser_file1.txt

echo "2. Creating directories and files..."
mkdir testuser_dir
touch testuser_dir/file_in_dir.txt
echo "nested content" > testuser_dir/file_in_dir.txt

echo "3. Creating nested directory structure..."
mkdir -p testuser_nested/a/b/c/d
touch testuser_nested/a/root_file.txt
touch testuser_nested/a/b/level2_file.txt
touch testuser_nested/a/b/c/level3_file.txt
touch testuser_nested/a/b/c/d/deep_file.txt

echo "4. Modifying files..."
echo "modified content" >> testuser_file1.txt
echo "more nested content" >> testuser_dir/file_in_dir.txt
echo "deep modification" >> testuser_nested/a/b/c/d/deep_file.txt

echo "5. Moving/renaming files..."
mv testuser_file1.txt testuser_file1_renamed.txt
mv testuser_dir/file_in_dir.txt testuser_dir/moved_file.txt

echo "6. Deleting files and directories..."
rm testuser_file1_renamed.txt
rm -rf testuser_dir
rm -rf testuser_nested

echo "7. Creating secret files..."
touch secrets/passwords.txt
echo "12345" > secrets/passwords.txt
rm -f secrets/passwords.txt

echo ""
echo "=== EVENTS GENERATED ==="
echo "All test operations completed."
echo "Let client capture events for a few more seconds..."
sleep 3

# Stop client
echo ""
echo "=== STOPPING CLIENT ==="
echo "Terminating client process..."
kill $CLIENT_PID 2>/dev/null
wait $CLIENT_PID 2>/dev/null
echo "Client stopped."

# Verify cleanup
echo ""
echo "=== VERIFYING CLEANUP ==="
echo "Remaining files in /tmp/vmtest created by testuser:"
ls -la /tmp/vmtest | grep testuser || echo "No testuser files remaining (good!)"

echo ""
echo "=== CLIENT TEST COMPLETE ==="
echo "Check the output above for captured filesystem events."
echo "You should see CREATE, MODIFY, DELETE, MOVE events with full path resolution."
echo ""
echo "The server is still running. To stop it, press Ctrl+C in the server terminal."
