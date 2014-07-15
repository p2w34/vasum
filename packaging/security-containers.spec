%define script_dir %{_sbindir}
# Security Containers Server's user info - it should already exist in the system
%define scs_user          security-containers
%define libvirt_group     libvirt
# The group that has read and write access to /dev/input/event* devices.
# It may vary between platforms.
%define input_event_group video

Name:           security-containers
Version:        0.1.0
Release:        0
Source0:        %{name}-%{version}.tar.gz
License:        Apache-2.0
Group:          Security/Other
Summary:        Daemon for managing containers
BuildRequires:  cmake
BuildRequires:  boost-devel
BuildRequires:  libvirt-devel
BuildRequires:  libjson-devel
BuildRequires:  libcap-ng-devel
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(libsystemd-journal)
BuildRequires:  pkgconfig(libvirt-glib-1.0)
Requires:       libvirt-daemon >= 1.2.4
Requires(post): libcap-tools

%description
This package provides a daemon used to manage containers - start, stop and switch
between them. A process from inside a container can request a switch of context
(display, input devices) to the other container.

%files
%manifest packaging/security-containers.manifest
%defattr(644,root,root,755)
%attr(755,root,root) %{_bindir}/security-containers-server
%dir /etc/security-containers
%dir /etc/security-containers/containers
%dir /etc/security-containers/libvirt-config
%config /etc/security-containers/daemon.conf
%config /etc/security-containers/containers/*.conf
%config /etc/security-containers/libvirt-config/*.xml
%{_unitdir}/security-containers.service
%{_unitdir}/multi-user.target.wants/security-containers.service
/etc/dbus-1/system.d/org.tizen.containers.host.conf

%prep
%setup -q

%build
%{!?build_type:%define build_type "RELEASE"}

%if %{build_type} == "DEBUG" || %{build_type} == "PROFILING"
    CFLAGS="$CFLAGS -Wp,-U_FORTIFY_SOURCE"
    CXXFLAGS="$CXXFLAGS -Wp,-U_FORTIFY_SOURCE"
%endif

%cmake . -DVERSION=%{version} \
         -DCMAKE_BUILD_TYPE=%{build_type} \
         -DSCRIPT_INSTALL_DIR=%{script_dir} \
         -DSYSTEMD_UNIT_DIR=%{_unitdir} \
         -DPYTHON_SITELIB=%{python_sitelib} \
         -DSECURITY_CONTAINERS_USER=%{scs_user} \
         -DLIBVIRT_GROUP=%{libvirt_group} \
         -DINPUT_EVENT_GROUP=%{input_event_group}
make -k %{?jobs:-j%jobs}

%install
%make_install
mkdir -p %{buildroot}/%{_unitdir}/multi-user.target.wants
ln -s ../security-containers.service %{buildroot}/%{_unitdir}/multi-user.target.wants/security-containers.service

%clean
rm -rf %{buildroot}

%post
# Refresh systemd services list after installation
if [ $1 == 1 ]; then
    systemctl daemon-reload || :
fi
# set needed caps on the binary to allow restart without loosing them
setcap CAP_SYS_ADMIN,CAP_MAC_OVERRIDE+ei %{_bindir}/security-containers-server

%preun
# Stop the service before uninstall
if [ $1 == 0 ]; then
     systemctl stop security-containers.service || :
fi

%postun
# Refresh systemd services list after uninstall/upgrade
systemctl daemon-reload || :
if [ $1 -ge 1 ]; then
    # TODO: at this point an appropriate notification should show up
    eval `systemctl show security-containers --property=MainPID`
    if [ -n "$MainPID" -a "$MainPID" != "0" ]; then
        kill -USR1 $MainPID
    fi
    echo "Security Containers updated. Reboot is required for the changes to take effect..."
else
    echo "Security Containers removed. Reboot is required for the changes to take effect..."
fi

## Client Package ##############################################################
%package client
Summary:          Security Containers Client
Group:            Development/Libraries
Requires:         security-containers = %{version}-%{release}
Requires(post):   /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description client
Library interface to the security-containers daemon

%files client
%manifest packaging/libsecurity-containers-client.manifest
%attr(644,root,root) %{_libdir}/libsecurity-containers-client.so


## Devel Package ###############################################################
%package devel
Summary:          Security Containers Client Devel
Group:            Development/Libraries
Requires:         security-containers = %{version}-%{release}
Requires:         security-containers-client = %{version}-%{release}

%description devel
Development package including the header files for the client library

%files devel
%manifest packaging/security-containers.manifest
%defattr(644,root,root,755)
%{_includedir}/security-containers
%{_libdir}/pkgconfig/*


## Container Support Package ###################################################
# TODO move to a separate repository
%package container-support
Summary:          Security Containers Support
Group:            Security/Other
Conflicts:        security-containers

%description container-support
Containers support installed inside every container.

%files container-support
%manifest packaging/security-containers-container-support.manifest
%defattr(644,root,root,755)
/etc/dbus-1/system.d/org.tizen.containers.domain.conf


## Container Daemon Package ####################################################
# TODO move to a separate repository
%package container-daemon
Summary:          Security Containers Containers Daemon
Group:            Security/Other
Requires:         security-containers-container-support = %{version}-%{release}

%description container-daemon
Daemon running inside every container.

%files container-daemon
%manifest packaging/security-containers-container-daemon.manifest
%defattr(644,root,root,755)
%attr(755,root,root) %{_bindir}/security-containers-container-daemon
/etc/dbus-1/system.d/org.tizen.containers.domain.daemon.conf


## Test Package ################################################################
%package tests
Summary:          Security Containers Tests
Group:            Development/Libraries
Requires:         security-containers = %{version}-%{release}
Requires:         security-containers-client = %{version}-%{release}
Requires:         python
Requires:         boost-test

%description tests
Unit tests for both: server and client and integration tests.

%files tests
%manifest packaging/security-containers-server-tests.manifest
%defattr(644,root,root,755)
%attr(755,root,root) %{_bindir}/security-containers-server-unit-tests
%attr(755,root,root) %{script_dir}/sc_all_tests.py
%attr(755,root,root) %{script_dir}/sc_int_tests.py
%attr(755,root,root) %{script_dir}/sc_launch_test.py
%{script_dir}/sc_test_parser.py
%{_datadir}/security-containers
%{python_sitelib}/sc_integration_tests
/etc/dbus-1/system.d/org.tizen.containers.tests.conf
