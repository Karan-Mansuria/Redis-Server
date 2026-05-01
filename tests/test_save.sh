#!/bin/bash
echo -e "=== Simulating Admin Client saving data ==="
(
    echo -en "AUTH admin admin123\r\n"; sleep 0.1
    echo -en "SET city Mumbai\r\n"; sleep 0.1
    echo -en "SAVE\r\n"; sleep 0.1
    echo -en "QUIT\r\n"
) | nc 127.0.0.1 6379
