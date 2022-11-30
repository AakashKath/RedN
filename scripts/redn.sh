git clone https://github.com/AakashKath/RedN.git
cd RedN/
sudo make
cd conf/
sudo ./disable_wqe_checks.sh mlx5_0
cd ../bench/micro/
sudo make
sudo ./hash_bench
