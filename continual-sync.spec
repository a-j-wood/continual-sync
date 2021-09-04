# Restrict to: RHEL5 RHEL6
Name:		continual-sync
Summary:	Tool to keep duplicates of directory trees continually in sync
Version:	0.0.5
Release:	1
License:	Artistic 2.0
Group:		System Environment/Tools
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
Requires:	rsync
Requires(post): /sbin/chkconfig
Requires(preun): /sbin/chkconfig
Requires(preun): /sbin/service
Requires(postun): /sbin/service
Source0:	%{name}-%{version}.tar.gz

%description
A tool to synchronise one directory tree with another, with rsync, using the
inotify mechanism to keep the copy constantly up to date very efficiently by
making rsync do as little work as possible.

%prep
%setup -q

%build
make CFLAGS="%{optflags}"

%install
mkdir -p ${RPM_BUILD_ROOT}
mkdir -p ${RPM_BUILD_ROOT}/var/log/%{name}
mkdir -p ${RPM_BUILD_ROOT}/var/run/%{name}
mkdir -p ${RPM_BUILD_ROOT}%{_sysconfdir}/logrotate.d
mkdir -p ${RPM_BUILD_ROOT}%{_sysconfdir}/%{name}.conf.d
mkdir -p ${RPM_BUILD_ROOT}%{_initrddir}
make install DESTDIR=${RPM_BUILD_ROOT} bindir=%{_bindir} mandir=%{_mandir}
cp continual-sync.init ${RPM_BUILD_ROOT}%{_initrddir}/%{name}
echo '/var/log/%{name}/*.log {' > ${RPM_BUILD_ROOT}%{_sysconfdir}/logrotate.d/%{name}
echo ' missingok' >> ${RPM_BUILD_ROOT}%{_sysconfdir}/logrotate.d/%{name}
echo ' notifempty' >> ${RPM_BUILD_ROOT}%{_sysconfdir}/logrotate.d/%{name}
echo '}' >> ${RPM_BUILD_ROOT}%{_sysconfdir}/logrotate.d/%{name}
echo 'include = /etc/%{name}.conf.d/*' > ${RPM_BUILD_ROOT}%{_sysconfdir}/%{name}.conf
cp defaults.cf ${RPM_BUILD_ROOT}%{_sysconfdir}/%{name}.conf.d/defaults

%clean
rm -rf $RPM_BUILD_ROOT

%post
if [ $1 -eq 1 ]; then
	#
	# Initial installation.
	#
	/sbin/chkconfig --add %{name}
else
	#
	# Upgrade or reinstall.
	#
	/sbin/service %{name} condrestart
fi

%preun
if [ "$1" = 0 ]; then
	/sbin/service %{name} stop >/dev/null 2>&1
	/sbin/chkconfig --del %{name}
fi

%files
%defattr(-,root,root,-)
%attr(0755,root,root) %{_bindir}/*
%attr(0755,root,root) %{_initrddir}/%{name}
%attr(0644,root,root) %{_mandir}/man1/*
%attr(0644,root,root) %{_mandir}/man5/*
%dir %attr(0700,root,root) /var/log/%{name}
%dir %attr(0755,root,root) /var/run/%{name}
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/%{name}.conf
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/%{name}.conf.d/defaults
%attr(0644,root,root) %config(noreplace) %{_sysconfdir}/logrotate.d/%{name}
%doc README NEWS COPYING example.cf example-large.cf

%changelog
* Fri Dec 12 2014 Andrew Wood <continual-sync@ivarch.com> 0.0.5-1
- Repackaged as an Open Source project with permission from my employer.

* Tue Aug 12 2014 Andrew Wood <continual-sync@ivarch.com> 0.0.4-1
- Added "working directory" field to status file.
- Check working directory still exists after every sync attempt, as well as
- when a watcher exits.

* Thu Aug  7 2014 Andrew Wood <continual-sync@ivarch.com> 0.0.3-1
- Added "current action" field to status file.

* Wed Aug  6 2014 Andrew Wood <continual-sync@ivarch.com> 0.0.2-1
- Added "temporary directory" parameter.
- Set default $PATH if one is not set in the environment.
- Record rsync errors in log file.
- Added "status file" parameter.

* Sat Aug  2 2014 Andrew Wood <continual-sync@ivarch.com> 0.0.1-1
- Initial package creation.
