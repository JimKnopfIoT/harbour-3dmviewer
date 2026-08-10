# Neutral packaging metadata — no personal identifiers (see CLAUDE.md anonymity rules).
Name:       harbour-3dmviewer
Summary:    Viewer for CAD models and sliced G-code
Version:    0.1.0
Release:    1
# ANONYMITY: neutral build host so built RPMs carry no real hostname/domain.
# Without this, the RPM BUILDHOST tag leaks the build machine's name and LAN
# domain into every published package.
%define _buildhost reproducible-builder
License:    GPL-3.0-or-later
URL:        https://github.com/JimKnopfIoT/harbour-3dmviewer
Source0:    %{name}-%{version}.tar.bz2
Vendor:     harbour-3dmviewer contributors
Packager:   harbour-3dmviewer contributors

Requires:   sailfishsilica-qt5
# Sailfish.Pickers lives in its own package; without it "Open model…" pushes a
# page that fails to load and the app looks broken. The name was verified on the
# device with `rpm -qf /usr/lib64/qt5/qml/Sailfish/Pickers/FilePickerPage.qml`.
Requires:   sailfish-components-pickers-qt5
# openNURBS is compiled in statically and brings its own zlib and uuid code, so
# reading .3dm adds no runtime dependency at all.
BuildRequires: pkgconfig(sailfishapp)
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt5Gui)
BuildRequires: pkgconfig(Qt5Sensors)
BuildRequires: desktop-file-utils

# The accelerometer backend is a runtime plugin that nothing links against, so
# rpm cannot infer it from the binary. Without it QAccelerometer constructs
# fine, reports no readings, and a dropped model would only ever fall straight
# down however the phone is held.
Requires:   qt5-qtsensors-plugin-sensorfw

%description
Opens Rhino (.3dm), STEP (.stp/.step), STL, 3MF and PrusaSlicer binary G-code
(.bgcode) and lets you turn them around: orbit, pinch to zoom, locked axis
views, and a switch per layer so you can peel a model apart. A sliced file also
gets a slider that removes print layers from the top and the bottom.

This is a viewer only: no editing and no save path of any
kind. It works entirely on the device and uses no network.

Rhino geometry comes from the render meshes stored in the file; models saved
with "Save Small" carry none and cannot be displayed. STEP faces on planes and
cylinders are tessellated on the device; other surface types are reported
rather than drawn. Whatever cannot be shown is said out loud.

Not affiliated with or endorsed by Robert McNeel & Associates. "Rhinoceros" and
the Rhino logo are their trademarks. Includes the openNURBS toolkit (zlib
license), which reads the Rhino format; the other readers are written for this
program. Not affiliated with or endorsed by Prusa Research.

%prep
%setup -q

%build
# VERSION/RELEASE reach the About page through DEFINES in src/src.pro.
%qmake5 VERSION=%{version} RELEASE=%{release}
%make_build

%install
%qmake5_install

desktop-file-install --delete-original \
    --dir %{buildroot}%{_datadir}/applications \
    %{buildroot}%{_datadir}/applications/*.desktop

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png

%changelog
* Mon Aug 10 2026 harbour-3dmviewer contributors 0.1.0-1
- First release: Rhino .3dm viewing, orbit/pan/zoom, per-layer visibility.
