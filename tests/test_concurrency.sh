#!/bin/bash

echo "Starting Concurrency Stress Test..."

# Reset the counter to 0 first
(echo "AUTH alice writer123"; sleep 0.1; echo "SET stress_counter 0"; sleep 0.1; echo "QUIT") | nc localhost 6379 > /dev/null

# Fire 100 INCR commands at the exact same time
for i in {1..100}
do
   (echo "AUTH alice writer123"; sleep 0.1; echo "INCR stress_counter"; sleep 0.1; echo "QUIT") | nc localhost 6379 > /dev/null &
done

# Wait for all background network requests to finish
wait

echo "Test complete! Fetching final counter value..."

# Get the final result
(echo "AUTH alice writer123"; sleep 0.1; echo "GET stress_counter"; sleep 0.1; echo "QUIT") | nc localhost 6379
