# StageRig for Fedora. Builds from the release tarball made by
# scripts/make-source-tarball.sh, which already carries JUCE and CLAP.
#
#   spectool -g packaging/rpm/stagerig.spec      # or put the tarball in ~/rpmbuild/SOURCES
#   rpmbuild -ba packaging/rpm/stagerig.spec

# Fedora has no clap-devel yet; the bundled CLAP headers are used unless
# built with --with system_clap.
%bcond system_clap 0

Name:           stagerig
Version:        0.1.57
Release:        1%{?dist}
Summary:        Live-performance plugin host for keyboard players

# StageRig itself is GPL-3.0-or-later; the bundled JUCE 8 is used under
# AGPL-3.0-only. The other bundled code is listed with bundled() below.
License:        GPL-3.0-or-later AND AGPL-3.0-only AND MIT AND ISC AND Apache-2.0 AND BSD-3-Clause AND Zlib AND IJG
URL:            https://github.com/dguedry/stagerig
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.xz

ExclusiveArch:  x86_64 aarch64

BuildRequires:  gcc-c++
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  pkgconfig
%if %{with system_clap}
BuildRequires:  clap-devel
%endif
BuildRequires:  alsa-lib-devel
BuildRequires:  pipewire-jack-audio-connection-kit-devel
BuildRequires:  libX11-devel
BuildRequires:  libXrandr-devel
BuildRequires:  libXinerama-devel
BuildRequires:  libXcursor-devel
BuildRequires:  libXext-devel
BuildRequires:  freetype-devel
BuildRequires:  fontconfig-devel
BuildRequires:  mesa-libGL-devel
BuildRequires:  ladspa-devel
BuildRequires:  desktop-file-utils
BuildRequires:  appstream
BuildRequires:  xorg-x11-server-Xvfb

# JUCE 8.0.15 is needed and is not packaged in Fedora; its own dependencies
# come with it, inside the JUCE modules.
Provides:       bundled(juce) = 8.0.15
%if %{without system_clap}
Provides:       bundled(clap)
%endif
Provides:       bundled(lilv)
Provides:       bundled(serd)
Provides:       bundled(sord)
Provides:       bundled(sratom)
Provides:       bundled(vst3sdk)
Provides:       bundled(harfbuzz)
Provides:       bundled(sheenbidi)
Provides:       bundled(flac)
Provides:       bundled(libvorbis)
Provides:       bundled(libjpeg)
Provides:       bundled(libpng)
Provides:       bundled(zlib)

Requires:       hicolor-icon-theme
Recommends:     pipewire-jack-audio-connection-kit
Recommends:     x11vnc
Recommends:     python3-websockify
Recommends:     novnc
Recommends:     xdotool
Recommends:     poppler-utils
Suggests:       lv2-calf-plugins

%description
StageRig hosts instruments and effects for playing live. Each keyboard has
128 programs selected by MIDI Program Change, each loading one or more VST3,
CLAP, LV2 or LADSPA instruments with key and velocity splits, insert effects
and MIDI controller mappings. Audio inputs carry effect chains, and every input
lands on an output with its own master effects.

Songs combine a program per keyboard with a tempo; set lists order the songs
for a footswitch to step through, with lead sheets and charts shown on a
tablet. A built-in web app lets a tablet or phone change programs and operate
any loaded plugin's own interface, mirrored over the network.

Every plugin runs in its own process, so a plugin that crashes or hangs takes
only itself down. Audio goes through PipeWire or JACK at low latency.

%prep
%autosetup -p1

%build
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSTAGERIG_VERSION=%{version} \
    -DSTAGERIG_SYSTEM_CLAP=%{?with_system_clap:ON}%{!?with_system_clap:OFF}
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.dguedry.StageRig.desktop
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/io.github.dguedry.StageRig.metainfo.xml
# The tests want a display. %%ctest expands to a compound command, which
# xvfb-run cannot wrap, so ctest is called directly on the build directory.
# The engine test reports itself skipped when no LV2 instrument is installed.
xvfb-run -a ctest --test-dir %{__cmake_builddir} --output-on-failure

%files
%license LICENSE
%doc README.md docs/MANUAL.md
%{_bindir}/StageRig
%{_bindir}/stagerig-plugin-host
%{_datadir}/applications/io.github.dguedry.StageRig.desktop
%{_metainfodir}/io.github.dguedry.StageRig.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/stagerig.*
%{_mandir}/man1/StageRig.1*
%{_mandir}/man1/stagerig-plugin-host.1*

%changelog
* Sun Oct 18 2026 Donnie Guedry <dguedry@gmail.com> - 0.1.57-1
- Initial package
