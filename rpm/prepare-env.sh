#!/bin/bash
# Install/get required packages on CentOS 7 and 8

set -e

DIST=`rpm --eval '%dist'`
rm -f /etc/yum.repos.d/CentOS-Media.repo
if [ "$DIST" = ".el8" ]; then
    dnf -y install centos-release-advanced-virtualization rpm-build
    dnf -y install gcc-toolset-9 gcc-toolset-9-gcc-c++ gperftools-devel qemu fio
    dnf download --disablerepo='*' --enablerepo='centos-advanced-virtualization-source' --source qemu-kvm
    dnf download --source fio
    rpm --nomd5 -i qemu*.src.rpm
    rpm --nomd5 -i fio*.src.rpm
    cd ~/rpmbuild/SPECS
    dnf builddep -y --enablerepo='*' --spec qemu-kvm.spec
    dnf builddep -y --enablerepo='*' --spec fio.spec
else
    yum -y --enablerepo=extras install centos-release-scl epel-release yum-utils rpm-build qemu fio
    yum -y --enablerepo='*' install devtoolset-9-gcc-c++ devtoolset-9-libatomic-devel gperftools-devel
    yumdownloader --source qemu
    yumdownloader --source fio
    rpm --nomd5 -i qemu*.src.rpm
    rpm --nomd5 -i fio*.src.rpm
    cd ~/rpmbuild/SPECS
    yum-builddep -y --enablerepo='*' qemu.spec
    yum-builddep -y --enablerepo='*' fio.spec
fi