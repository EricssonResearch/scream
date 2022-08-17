set -v
sudo sysctl -w net.core.rmem_max=26214400
sudo sysctl -w net.core.rmem_max=26214400
sudo sysctl -w net.ipv4.udp_rmem_min=409600

sudo sysctl -w net.core.wmem_max=26214400
sudo sysctl -w net.core.wmem_default=26214400
sudo sysctl -w net.core.rmem_max=26214400
sudo sysctl -w net.core.rmem_default=26214400
