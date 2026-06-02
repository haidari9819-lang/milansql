Name:           milansql
Version:        5.7.0
Release:        1%{?dist}
Summary:        Production-grade SQL database engine
License:        MIT
URL:            https://github.com/haidari9819-lang/milansql
Source0:        https://github.com/haidari9819-lang/milansql/archive/v%{version}.tar.gz

BuildRequires:  cmake >= 3.16
BuildRequires:  gcc-c++ >= 9
BuildRequires:  ninja-build
BuildRequires:  systemd-rpm-macros

Requires:       glibc >= 2.17
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd

%description
MilanSQL is a production-grade relational database engine built from
scratch in C++17 with zero external dependencies. It supports SQL-92
syntax, MVCC transactions, WAL crash recovery, full-text search, JSON,
time-series, and a built-in HTTP/GraphQL API.

Features:
- ACID transactions with MVCC and WAL crash recovery
- Double-Write Buffer for torn-page protection
- MySQL and PostgreSQL wire protocol compatibility
- Built-in HTTP REST and GraphQL APIs
- Native C++17, zero external dependencies

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DCMAKE_BUILD_TYPE=Release -G Ninja
%cmake_build

%install
install -D -m 755 %{_vpath_builddir}/milansql \
    %{buildroot}%{_bindir}/milansql

install -D -m 644 packaging/debian/milansql.service \
    %{buildroot}%{_unitdir}/milansql.service

install -d -m 755 %{buildroot}%{_localstatedir}/lib/milansql
install -d -m 755 %{buildroot}%{_localstatedir}/log/milansql

install -D -m 644 README.md \
    %{buildroot}%{_docdir}/%{name}/README.md
install -D -m 644 LICENSE \
    %{buildroot}%{_docdir}/%{name}/LICENSE

%pre
# Create system user
getent passwd milansql >/dev/null || \
    useradd -r -s /sbin/nologin -d %{_localstatedir}/lib/milansql milansql
exit 0

%post
%systemd_post milansql.service

%preun
%systemd_preun milansql.service

%postun
%systemd_postun_with_restart milansql.service

%files
%license LICENSE
%doc README.md INSTALL.md
%{_bindir}/milansql
%{_unitdir}/milansql.service
%dir %attr(750, milansql, milansql) %{_localstatedir}/lib/milansql
%dir %attr(750, milansql, milansql) %{_localstatedir}/log/milansql

%changelog
* Mon Jun 02 2026 Mirwais Haidari <haidari9819@gmail.com> - 5.7.0-1
- Phase 115: Kubernetes Operator, Helm Chart, APT/RPM packaging, Windows MSI
- Phase 114: Crash Recovery V2 + Double-Write Buffer + CHECK TABLE
- Phase 113: DP Query Planner V2 + Histogramme + EXPLAIN ANALYZE V2
- Phase 112: Lock-free B-Tree + RwLock per Table
