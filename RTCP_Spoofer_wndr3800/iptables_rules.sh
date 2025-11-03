# add rule
iptables -t mangle -I PREROUTING -i wlan0 -j NFQUEUE --queue-num 0

#remove rule
iptables -t mangle -D PREROUTING -i wlan0 -j NFQUEUE --queue-num 0