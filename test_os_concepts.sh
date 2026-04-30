#!/bin/bash

HOST="127.0.0.1"
PORT="6379"

echo "==============================================="
echo "   Mini-Redis OS Concept Evaluation Script     "
echo "==============================================="
echo ""

echo "▶ TEST 1: Role-Based Authorization (OS Concept 1)"
echo "--- 1A. Reader Test (Bob) ---"
echo "Attempting to SET data (Should fail):"
{ echo "AUTH bob reader123"; sleep 0.1; echo "SET top_secret 100"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT
echo ""

echo "--- 1B. Writer Test (Alice) ---"
echo "Attempting to SET data (Should succeed) and DEL data (Should fail):"
{ echo "AUTH alice writer123"; sleep 0.1; echo "SET top_secret 100"; sleep 0.1; echo "DEL top_secret"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT
echo ""

echo "--- 1C. Admin Test (Admin) ---"
echo "Attempting to DEL data (Should succeed):"
{ echo "AUTH admin admin123"; sleep 0.1; echo "DEL top_secret"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT
echo ""


echo "▶ TEST 2: Background Expiry Thread"
echo "Setting 'bomb' to expire in 2 seconds..."
{ echo "AUTH admin admin123"; sleep 0.1; echo "SET bomb tick-tock"; sleep 0.1; echo "EXPIRE bomb 2"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT

echo ""
echo "Waiting 3 seconds for the background daemon to wake up and purge the memory..."
sleep 3

echo "Fetching 'bomb' (Should return nil):"
{ echo "AUTH admin admin123"; sleep 0.1; echo "GET bomb"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT
echo ""


echo "▶ TEST 3: Persistence & IPC (OS Concepts 2 & 6)"
echo "Setting final grade and triggering SAVE / BGSAVE..."
{ echo "AUTH admin admin123"; sleep 0.1; echo "SET project_grade A+"; sleep 0.1; echo "BGSAVE"; sleep 0.1; echo "SAVE"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT
echo ""

echo "==============================================="
echo "  Client-Side Tests Complete! "
echo "  Now check your Server Terminal to see the "
echo "    BGSAVE Fork(), Pipe, and File Lock logs!"
echo "==============================================="
