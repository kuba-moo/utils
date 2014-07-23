#!/bin/bash

error()
{
    echo -e "\e[31;1m"Something went wrong!

    exit 1
}

usage() 
{
    cat <<EOF
 Install kernel.
 Kernel should be already built and you should be in the source directory.
    -h Display this message
    -V Overwrite automatically detected version
    -O Set output dir
EOF

    exit 0
}

[ ! -e MAINTAINERS ] && echo "You are not in linux tree" && exit 1
[ ! `whoami` == "root" ] && echo "You need to be root" && exit 2 

_MAKEFLAGS="$MAKEFLAGS"

while getopts ":hV:O:" Option
do
    case $Option in
	h) usage ;;
	V) VERSION="$OPTARG" ;;
	O) export MAKEFLAGS="O=${OPTARG}"; BDIR="$OPTARG" ;;
	*) echo "Unimplemented option chosen" ;;
    esac
done

[ -z "$VERSION" ] && VERSION=`make kernelrelease | tail -1`

echo -e "\e[1mVersion: $VERSION Makeflags: $MAKEFLAGS\e[0m"

echo -e "\e[1mInstalling modules\e[0m"

make modules_install

echo -e "\e[1mCopying stuff to /boot\e[0m"

cp ./${BDIR}/arch/x86_64/boot/bzImage /boot/vmlinuz-${VERSION} || error
cp ./${BDIR}/System.map /boot/System.map-${VERSION} || error
cp ./${BDIR}/.config /boot/config-${VERSION} || error

echo -e "\e[1mBuilding initramfs\e[0m"

(
    cd /boot
    dracut --force initramfs-${VERSION}.img ${VERSION} || error
)

echo -e "\e[1mMaking GRUB happy\e[0m"
grub2-mkconfig > /etc/grub2.cfg || error

grub2-set-default "Fedora, with Linux ${VERSION}" || error

# Restore things we've changed
export MAKEFLAGS="$_MAKEFLAGS"
