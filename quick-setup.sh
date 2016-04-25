#!/bin/bash

if python -mplatform | grep -q -i fedora; then
  sudo dnf install -y libgcrypt-devel gnutls-devel git
elif python -mplatform | grep -q -i ubuntu; then
  sudo apt-get install -y libgcrypt-devel gnutls-devel
else
  echo "Only Fedora and Ubuntu are supported. Open up the source and try to get it working on your distribution (contribute back when you get it working.)";
  exit 1;
fi

git clone git@github.com:hekar/vpnc-fortisgate.git
cd vpnc-fortisgate/vpnc
make
cd ..
