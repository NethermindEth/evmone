#!/bin/bash

# Install Docker 
sudo apt -y install apt-transport-https ca-certificates curl gnupg lsb-release
echo   "deb [arch=amd64 signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/ubuntu \
  $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update
sudo apt -y install docker-ce docker-ce-cli containerd.io
newgrp docker 

# Clone dependencies
cd ~
git clone --recursive https://github.com/NethermindEth/evmone

# Install linux-sgx
sudo apt update
sudo apt -y install build-essential ocaml ocamlbuild automake autoconf libtool wget python-is-python3 libssl-dev git cmake perl libssl-dev libcurl4-openssl-dev protobuf-compiler libprotobuf-dev debhelper cmake reprepro unzip

cd ~/linux-sgx && make preparation
sudo cp external/toolset/ubuntu20.04/* /usr/local/bin
make sdk
make sdk USE_OPT_LIBS=1
make sdk_install_pkg
sudo ./linux/installer/bin/sgx_linux_x64_sdk_2.13.103.1.bin
source /opt/intel/sgxsdk/environment 
make psw
make deb_psw_pkg
make deb_local_repo
cd ~/linux-sgx/linux/installer/deb/sgx_debian_local_repo
sudo dpkg -R --install ./
sudo apt-get --fix-broken install
sudo apt -y install dkms

# Install Mystikos
cd ~
RELEASE="mystikos-${LATEST}-x86_64"
LATEST='0.1.2'
RELEASE="mystikos-${LATEST}-x86_64"
curl -sSL --ssl https://github.com/deislabs/mystikos/releases/download/v${LATEST}/${RELEASE}.tar.gz | tar -xzf -
export PATH="$PATH:$(pwd)/mystikos/bin"
cd evmone/
myst-appbuilder Dockerfile.alpine 
myst mkcpio appdir evmone
