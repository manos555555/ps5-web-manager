#!/bin/bash

echo "================================================"
echo "PS5 Web Manager - Compilation"
echo "By Manos"
echo "================================================"
echo ""

# Set PS5 SDK path
export PS5_PAYLOAD_SDK="/home/hackman/ps5sdk_copy"

# Check if SDK exists
if [ ! -d "$PS5_PAYLOAD_SDK" ]; then
    echo "[-] Error: PS5 SDK not found at $PS5_PAYLOAD_SDK"
    exit 1
fi

echo "[+] Compiling..."
make clean
make

if [ $? -eq 0 ]; then
    echo ""
    echo "================================================"
    echo "[+] SUCCESS! Compiled ps5_web_manager.elf"
    echo "================================================"
    echo ""
    ls -lh ps5_web_manager.elf
    file ps5_web_manager.elf
    echo ""
    echo "Upload to PS5 and run with elfldr"
    echo "Access web interface at: http://PS5_IP:8080"
else
    echo ""
    echo "[-] Compilation failed!"
    exit 1
fi
