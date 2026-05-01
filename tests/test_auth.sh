#!/bin/bash
echo -e "\n=== Testing READER Role (Bob) ==="
(
    echo -en "AUTH bob reader123\r\n"; sleep 0.1
    echo -en "SET name value\r\n"; sleep 0.1
    echo -en "QUIT\r\n"
) | nc 127.0.0.1 6379

echo -e "\n=== Testing WRITER Role (Alice) ==="
(
    echo -en "AUTH alice writer123\r\n"; sleep 0.1
    echo -en "DEL name\r\n"; sleep 0.1
    echo -en "QUIT\r\n"
) | nc 127.0.0.1 6379

echo -e "\n=== Testing ADMIN Role (Admin) ==="
(
    echo -en "AUTH admin admin123\r\n"; sleep 0.1
    echo -en "DEL name\r\n"; sleep 0.1
    echo -en "QUIT\r\n"
) | nc 127.0.0.1 6379
echo ""
