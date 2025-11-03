#!/bin/bash
user="admin"
pw="adminadmin"

ROUTER_IP="192.168.2.2"

# Compile (produces ARM executable)
rm csi_analyzer
aarch64-linux-gnu-gcc src/csi_analyzer.c -o csi_analyzer -lm -O3 --static
sshpass -p ${pw} ssh "${user}@${ROUTER_IP}" "rm /jffs/csi_analyzer"
sshpass -p ${pw} scp csi_analyzer ${user}@${ROUTER_IP}:/jffs/
echo "copying done"

# Execute on router (key steps below)
sshpass -p "${pw}" ssh "${user}@${ROUTER_IP}" "chmod +x /jffs/csi_analyzer"
# && /jffs/csi_analyzer"
