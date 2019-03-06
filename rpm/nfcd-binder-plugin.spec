Name: nfcd-binder-plugin
Version: 1.0.0
Release: 0
Summary: Binder-based NFC plugin
Group: Development/Libraries
License: BSD
URL: https://github.com/mer-hybris/nfcd-binder-plugin
Source: %{name}-%{version}.tar.bz2
BuildRequires: pkgconfig(libgbinder) >= 1.0.30
BuildRequires: pkgconfig(nfcd-plugin) >= 1.0.8
Requires: libgbinder >= 1.0.30
Requires: nfcd

%description
Binder-based NFC plugin for Android 8+.

%prep
%setup -q

%build
make %{_smp_mflags} KEEP_SYMBOLS=1 release

%check
make -C unit test

%install
rm -rf %{buildroot}
make install DESTDIR=%{buildroot}

%post
systemctl reload-or-try-restart nfcd.service ||:

%postun
systemctl reload-or-try-restart nfcd.service ||:

%files
%defattr(-,root,root,-)
%dir %{_libdir}/nfcd/plugins
%{_libdir}/nfcd/plugins/*.so
