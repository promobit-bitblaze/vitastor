Name:           vitastor
Version:        0.5.4
Release:        2%{?dist}
Summary:        Vitastor, a fast software-defined clustered block storage

License:        Vitastor Network Public License 1.0
URL:            https://vitastor.io/
Source0:        vitastor-0.5.4.el7.tar.gz

BuildRequires:  liburing-devel >= 0.6
BuildRequires:  gperftools-devel
BuildRequires:  devtoolset-9-gcc-c++
BuildRequires:  rh-nodejs12
BuildRequires:  rh-nodejs12-npm
BuildRequires:  jerasure-devel
BuildRequires:  gf-complete-devel
Requires:       fio = 3.7-1.el7
Requires:       qemu-kvm = 2.0.0-1.el7.6
Requires:       rh-nodejs12
Requires:       rh-nodejs12-npm
Requires:       liburing >= 0.6
Requires:       libJerasure2
Requires:       lpsolve

%description
Vitastor is a small, simple and fast clustered block storage (storage for VM drives),
architecturally similar to Ceph which means strong consistency, primary-replication,
symmetric clustering and automatic data distribution over any number of drives of any
size with configurable redundancy (replication or erasure codes/XOR).


%prep
%setup -q


%build
. /opt/rh/devtoolset-9/enable
make %{?_smp_mflags} BINDIR=%_bindir LIBDIR=%_libdir QEMU_PLUGINDIR=%_libdir/qemu-kvm


%install
rm -rf $RPM_BUILD_ROOT
%make_install BINDIR=%_bindir LIBDIR=%_libdir QEMU_PLUGINDIR=%_libdir/qemu-kvm
. /opt/rh/rh-nodejs12/enable
cd mon
npm install
cd ..
mkdir -p %buildroot/usr/lib/vitastor
cp -r mon %buildroot/usr/lib/vitastor/mon


%files
%doc
%_bindir/vitastor-dump-journal
%_bindir/vitastor-nbd
%_bindir/vitastor-osd
%_bindir/vitastor-rm
%_libdir/qemu-kvm/block-vitastor.so
%_libdir/vitastor
/usr/lib/vitastor


%changelog
