#!/bin/bash

# Test script for QUIC-simple pub/sub system
# Run this with: bash test_quic_simple.sh

set -e

echo "=================================="
echo "Testing QUIC-Simple Pub/Sub System"
echo "=================================="
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Kill any existing processes
pkill -f "broker_quic_simple" || true
pkill -f "subscriber_quic_simple" || true
pkill -f "publisher_quic_simple" || true
sleep 1

echo "[1/3] Starting Broker..."
./output/broker_quic_simple &
BROKER_PID=$!
sleep 1

echo "[2/3] Starting Subscriber (subscribing to AB, CD)..."
echo -e "1\nAB CD" | ./output/subscriber_quic_simple &
SUBSCRIBER_PID=$!
sleep 2

echo "[3/3] Starting Publisher (publishing to AB)..."
echo -e "A B" | ./output/publisher_quic_simple &
PUBLISHER_PID=$!

# Wait for publisher to finish
wait $PUBLISHER_PID 2>/dev/null || true
sleep 2

# Kill remaining processes
kill $SUBSCRIBER_PID 2>/dev/null || true
kill $BROKER_PID 2>/dev/null || true

echo ""
echo "=================================="
echo "Test Complete"
echo "=================================="
