#!/bin/bash

# Interactive test for QUIC-simple pub/sub system

pkill -f 'broker_quic_simple' || true
pkill -f 'subscriber_quic_simple' || true
pkill -f 'publisher_quic_simple' || true
sleep 1

echo "=== Starting Broker ==="
./output/broker_quic_simple &
BROKER_PID=$!
sleep 2

echo "=== Starting Subscriber ==="
timeout 15 bash -c 'echo -e "1\nAB" | ./output/subscriber_quic_simple' &
SUBSCRIBER_PID=$!
sleep 3

echo "=== Starting Publisher ==="
echo -e "A B" | ./output/publisher_quic_simple
sleep 2

# Give subscriber time to receive events
sleep 3

# Kill processes
kill $SUBSCRIBER_PID 2>/dev/null || true
kill $BROKER_PID 2>/dev/null || true

echo "=== Test Complete ==="
