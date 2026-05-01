#!/bin/bash

HOST="127.0.0.1"
PORT="6379"

# The Ultimate Filter: Extracts ONLY the exact server responses and ignores all menus
clean_output() {
    grep -oE "(\+OK|-ERR|Success|Failed|Result|Value|\(nil\)).*" | sed 's/\r//'
}

echo "======================================================="
echo " 🚀 MINI-REDIS ULTIMATE MASTER EVALUATION SCRIPT 🚀"
echo "======================================================="
echo "Ensuring server is reachable..."
sleep 1

# ---------------------------------------------------------
echo -e "\n▶ PHASE 1: Basic Commands (SET, GET, APPEND, EXISTS)"
# ---------------------------------------------------------
echo "Alice (Writer) is setting a key, appending to it, and checking it..."
{ 
  echo "AUTH alice writer123"; sleep 0.1; 
  echo "SET msg Hello"; sleep 0.1; 
  echo "APPEND msg _World"; sleep 0.1; 
  echo "EXISTS msg"; sleep 0.1;
  echo "GET msg"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

# ---------------------------------------------------------
echo -e "\n▶ PHASE 2: Role-Based Authorization (OS Concept 1)"
# ---------------------------------------------------------
echo "❌ Bob (Reader) trying to SET data (Should be DENIED):"
{ 
  echo "AUTH bob reader123"; sleep 0.1; 
  echo "SET hack 123"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

echo "❌ Alice (Writer) trying to DEL data (Should be DENIED):"
{ 
  echo "AUTH alice writer123"; sleep 0.1; 
  echo "DEL msg"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

echo "✅ Admin trying to DEL data (Should SUCCEED):"
{ 
  echo "AUTH admin admin123"; sleep 0.1; 
  echo "DEL msg"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

# ---------------------------------------------------------
echo -e "\n▶ PHASE 3: TTL and Background Expiry Thread"
# ---------------------------------------------------------
echo "Admin is creating a 'bomb' with a 2-second TTL..."
{ 
  echo "AUTH admin admin123"; sleep 0.1; 
  echo "SET bomb ticktock"; sleep 0.1; 
  echo "EXPIRE bomb 2"; sleep 0.1; 
  echo "TTL bomb"; sleep 0.1;
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

echo "⏳ Waiting 3 seconds for the background daemon to destroy the bomb..."
sleep 3

echo "Fetching 'bomb' (Should return nil / Not Found):"
{ 
  echo "AUTH admin admin123"; sleep 0.1; 
  echo "GET bomb"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

# ---------------------------------------------------------
echo -e "\n▶ PHASE 4: Concurrency & Data Consistency (OS Concepts 3 & 4)"
# ---------------------------------------------------------
echo "Setting 'stress_counter' to 0..."
{ echo "AUTH admin admin123"; sleep 0.1; echo "SET stress_counter 0"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT > /dev/null

echo "🔥 Firing 100 simultaneous INCR commands to test the Mutex..."
for i in {1..100}
do
   { echo "AUTH alice writer123"; sleep 0.1; echo "INCR stress_counter"; sleep 0.1; echo "QUIT"; sleep 0.1; } | nc $HOST $PORT > /dev/null &
done

# Wait for all 100 background processes to finish
wait 

echo "✅ Final Counter Value (Should be exactly 100):"
{ 
  echo "AUTH admin admin123"; sleep 0.1; 
  echo "GET stress_counter"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

# ---------------------------------------------------------
echo -e "\n▶ PHASE 5: IPC & Persistence (OS Concepts 2 & 6)"
# ---------------------------------------------------------
echo "Triggering BGSAVE (fork + pipe) and SAVE (file locking)..."
{ 
  echo "AUTH admin admin123"; sleep 0.1; 
  echo "BGSAVE"; sleep 0.1; 
  echo "SAVE"; sleep 0.1; 
  echo "QUIT"; sleep 0.1; 
} | nc $HOST $PORT | clean_output

echo ""
echo "======================================================="
echo " 🎉 ALL TESTS COMPLETED! "
echo " 👉 LOOK AT YOUR SERVER TERMINAL NOW!"
echo " You should see logs for 100 connections, a Forked Child,"
echo " IPC pipe communication, and a successful dump.rdb save!"
echo "======================================================="
