Summary: EFI boot manager software
Name: refind
Version: 0.8.4
Release: 1%{?dist}
Summary: EFI boot manager software
License: GPLv3
URL: http://www.rodsbooks.com/refind/
Group: System Environment/Base
Source: refind-src-%version.zip
Requires: efibootmgr
BuildRoot: %(mktemp -ud %{_tmppath}/%{name}-%{version}-%{release}-XXXXXX)

%define efiarch unknown
%ifarch i386
%define efiarch ia32
%endif
%ifarch i486
%define efiarch ia32
%endif
%ifarch i586
%define efiarch ia32
%endif
%ifarch i686
%define efiarch ia32
%endif
%ifarch x86_64
%define efiarch x64
%endif

# Directory in which refind.key and refind.crt files are found for
# signing of binaries. If absent, binaries are copied unsigned.
%define keydir /mnt/refind

%description

A graphical boot manager for EFI- and UEFI-based computers, such as all
Intel-based Macs and recent (most 2011 and later) PCs. rEFInd presents a
boot menu showing all the EFI boot loaders on the EFI-accessible
partitions, and optionally BIOS-bootable partitions on Macs. EFI-compatbile
OSes, including Linux, provide boot loaders that rEFInd can detect and
launch. rEFInd can launch Linux EFI boot loaders such as ELILO, GRUB
Legacy, GRUB 2, and 3.3.0 and later kernels with EFI stub support. EFI
filesystem drivers for ext2/3/4fs, ReiserFS, HFS+, and ISO-9660 enable
rEFInd to read boot loaders from these filesystems, too. rEFInd's ability
to detect boot loaders at runtime makes it very easy to use, particularly
when paired with Linux kernels that provide EFI stub support.

%prep
%setup -q

%build
if [[ -d /usr/local/UDK2014 ]] ; then
   make
   make fs
else
   make gnuefi
   make fs_gnuefi
fi

%install
rm -rf $RPM_BUILD_ROOT
mkdir -p $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/

# Copy the rEFInd binaries (rEFInd proper and drivers) to /usr/share/refind-%{version},
# including signing the binaries if sbsign is installed and a %{keydir}/refind.key file
# is available
declare SBSign=`which sbsign 2> /dev/null`
if [[ -f %{keydir}/refind.key && -x $SBSign ]] ; then
   $SBSign --key %{keydir}/refind.key --cert %{keydir}/refind.crt --output $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/refind_%{efiarch}.efi refind/refind_%{efiarch}.efi
   mkdir -p $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/drivers_%{efiarch}
   for File in `ls drivers_%{efiarch}/*_x64.efi` ; do
      $SBSign --key %{keydir}/refind.key --cert %{keydir}/refind.crt --output $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/$File $File
   done
   mkdir -p $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/tools_%{efiarch}
   $SBSign --key %{keydir}/refind.key --cert %{keydir}/refind.crt --output $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/tools_%{efiarch}/gptsync_%{efiarch}.efi gptsync/gptsync_%{efiarch}.efi
else
   install -Dp -m0644 refind/refind*.efi $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/
   mkdir -p $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/drivers_%{efiarch}
   cp -a drivers_%{efiarch}/* $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/drivers_%{efiarch}/
   mkdir -p $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/tools_%{efiarch}
   install -Dp -m0644 gptsync/gptsync_%{efiarch}.efi $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/tools_%{efiarch}/gptsync_%{efiarch}.efi
fi

# Copy configuration and support files to /usr/share/refind-%{version}
install -Dp -m0644 refind.conf-sample $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/
cp -a icons $RPM_BUILD_ROOT/usr/share/refind-%{version}/refind/
install -Dp -m0755 install.sh $RPM_BUILD_ROOT/usr/share/refind-%{version}/

# Copy documentation to /usr/share/doc/refind-%{version}
mkdir -p $RPM_BUILD_ROOT/usr/share/doc/refind-%{version}
cp -a docs/* $RPM_BUILD_ROOT/usr/share/doc/refind-%{version}/
install -Dp -m0644 NEWS.txt COPYING.txt LICENSE.txt README.txt CREDITS.txt $RPM_BUILD_ROOT/usr/share/doc/refind-%{version}

# Copy keys to /etc/refind.d/keys
mkdir -p $RPM_BUILD_ROOT/etc/refind.d/keys
install -Dp -m0644 keys/* $RPM_BUILD_ROOT/etc/refind.d/keys

# Copy scripts to /usr/sbin
mkdir -p $RPM_BUILD_ROOT/usr/sbin
install -Dp -m0755 mkrlconf.sh $RPM_BUILD_ROOT/usr/sbin/
install -Dp -m0755 mvrefind.sh $RPM_BUILD_ROOT/usr/sbin/

# Copy banners and fonts to /usr/share/refind-%{version}
cp -a banners $RPM_BUILD_ROOT/usr/share/refind-%{version}/
cp -a fonts $RPM_BUILD_ROOT/usr/share/refind-%{version}/

%clean
#rm -rf $RPM_BUILD_ROOT

%files
%defattr(-,root,root -)
%doc /usr/share/doc/refind-%{version}
/usr/sbin/mkrlconf.sh
/usr/sbin/mvrefind.sh
/usr/share/refind-%{version}
/etc/refind.d/

%post
PATH=$PATH:/usr/local/bin
# Remove any existing NVRAM entry for rEFInd, to avoid creating a duplicate.
ExistingEntry=`efibootmgr | grep "rEFInd Boot Manager" | cut -c 5-8`
if [[ -n $ExistingEntry ]] ; then
   efibootmgr --bootnum $ExistingEntry --delete-bootnum
fi

cd /usr/share/refind-%{version}

declare VarFile=`ls -d /sys/firmware/efi/vars/SecureBoot* 2> /dev/null`
# Note: Two find operations for ShimFile favors shim over PreLoader -- if both are
# present, the script uses shim rather than PreLoader.
declare ShimFile=`find /boot -name shim\.efi -o -name shimx64\.efi -o -name PreLoader\.efi 2> /dev/null | head -n 1`
if [[ ! -n $ShimFile ]] ; then
   declare ShimFile=`find /boot -name PreLoader\.efi 2> /dev/null | head -n 1`
fi
declare SBSign=`which sbsign 2> /dev/null`
declare OpenSSL=`which openssl 2> /dev/null`

# Run the rEFInd installation script. Do so with the --shim option
# if Secure Boot mode is suspected and if a shim program can be
# found, or without it if not. If a shim installation is attempted
# and the sbsign and openssl programs can be found, do the install
# using a local signing key. Note that this option is undesirable
# for a distribution, since it would then require the user to
# enroll an extra MOK. I'm including it here because I'm NOT a
# distribution maintainer, and I want to encourage users to use
# their own local keys.
if [[ -n $VarFile && -n $ShimFile ]] ; then
   if [[ -n $SBSign && -n $OpenSSL ]] ; then
      ./install.sh --shim $ShimFile --localkeys --yes
   else
      ./install.sh --shim $ShimFile --yes
   fi
else
   ./install.sh --yes
fi

# CAUTION: Don't create a %preun or a %postun script that deletes the files
# installed by install.sh, since that script will run after an update, thus
# wiping out the just-updated files.

%changelog
* Mon Dec 8 2014 R Smith <rodsmith@rodsbooks.com> - 0.8.4
- Updated spec file for 0.8.4
* Sun Jul 6 2014 R Smith <rodsmith@rodsbooks.com> - 0.8.3
- Updated spec file for 0.8.3
* Sun Jun 8 2014 R Smith <rodsmith@rodsbooks.com> - 0.8.2
- Updated spec file for 0.8.2
* Thu May 15 2014 R Smith <rodsmith@rodsbooks.com> - 0.8.1
- Updated spec file for 0.8.1
* Sun May 4 2014 R Smith <rodsmith@rodsbooks.com> - 0.8.0
- Updated spec file for 0.8.0
* Sun Apr 20 2014 R Smith <rodsmith@rodsbooks.com> - 0.7.9
- Updated spec file for 0.7.9
* Sun Mar 9 2014 R Smith <rodsmith@rodsbooks.com> - 0.7.8
- Updated spec file for 0.7.8
* Fri Jan 3 2014 R Smith <rodsmith@rodsbooks.com> - 0.7.7
- Created spec file for 0.7.7 release
