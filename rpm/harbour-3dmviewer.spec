# Neutral packaging metadata — no personal identifiers (see CLAUDE.md anonymity rules).
Name:       harbour-3dmviewer
Summary:    Viewer for CAD models and sliced G-code
Version:    0.1.3
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
# openNURBS is compiled into libharbour-3dmviewer-rhino.so, which this package
# ships in a private directory under the library path and the application
# dlopens the first time a .3dm is opened. It brings its own zlib and uuid code, so reading
# .3dm adds no external runtime dependency; and because it is loaded on demand,
# a user who never opens a Rhino file never loads it at all.
BuildRequires: pkgconfig(sailfishapp)
BuildRequires: pkgconfig(Qt5Core)
BuildRequires: pkgconfig(Qt5Qml)
BuildRequires: pkgconfig(Qt5Quick)
BuildRequires: pkgconfig(Qt5Gui)
BuildRequires: pkgconfig(Qt5Sensors)
# The 3MF and bgcode readers inflate with the system zlib.
BuildRequires: pkgconfig(zlib)
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
the Rhino logo are their trademarks. Includes McNeel's openNURBS toolkit, which
reads the Rhino format and is redistributed under the terms McNeel publish for
it; the other readers are written for this program. Not affiliated with or
endorsed by Prusa Research.

%prep
%setup -q

%build
# VERSION/RELEASE reach the About page through DEFINES in src/src.pro.
%qmake5 VERSION=%{version} RELEASE=%{release}
%make_build

%install
%qmake5_install

# rpmbuild's strip pass takes the executable and leaves this one alone — it
# only walks the standard binary directories, and this object sits in a private
# subdirectory of them. Unstripped it is 67 MB of debug information, which
# would be six times the rest of the package put together. So strip it here,
# where it is one visible line rather than a surprise in the package size.
strip --strip-unneeded %{buildroot}%{_libdir}/%{name}/lib%{name}-rhino.so

desktop-file-install --delete-original \
    --dir %{buildroot}%{_datadir}/applications \
    %{buildroot}%{_datadir}/applications/*.desktop

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
# The Rhino reader, loaded on demand. Private to this application: nothing else
# may link or dlopen it, which is why it is here and not in %{_libdir} proper.
%dir %{_libdir}/%{name}
%{_libdir}/%{name}/lib%{name}-rhino.so
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png

%changelog
* Sun Aug 30 2026 harbour-3dmviewer contributors 0.1.3-1
- Fix the startup crash on devices with a newer Android base (reproduced on a
  Jolla J2). openNURBS present in the executable image killed the EGL setup
  before the first frame; the Rhino reader now lives in a shared object loaded
  only when a .3dm is opened, which is measurably safe and means the other four
  formats never pay for it. The 0.1.2 symbol-export fix was a real defect but
  not this one.
- Fix an empty screen on Mali GPUs. A full-screen multisampled render target is
  allocated lazily there, so it reported itself complete and then failed every
  draw with GL_OUT_OF_MEMORY. Multisampling now steps down when the driver
  refuses it, the affected buffers are uploaded again, and the result is
  remembered so the next start is clean.
- New: one button in the viewer's toolbar cycles how a drag turns the model —
  about the screen centre, about a point tapped on the model, or freely with a
  two-finger roll. Pressing it once more restores the opening view.
- Keep the header strip and the two simulation pages' hints clear of a camera
  cutout.
- The 3MF and bgcode readers use the system zlib instead of the copy that came
  with openNURBS.
- Drop -mtune=cortex-a53: it named the development phone's core.

* Tue Aug 11 2026 harbour-3dmviewer contributors 0.1.2-1
- Link with --exclude-libs,ALL. sailfishapp's -rdynamic was exporting all
  22725 symbols that the static openNURBS archive contributed into the
  executable's dynamic symbol table, where they preempted the system libz
  and libuuid for every library loaded afterwards. Addresses a startup crash
  reported on a device other than the development one, whose trace ends in
  the hybris linker; not confirmed to be that crash, but a real defect.

* Tue Aug 11 2026 harbour-3dmviewer contributors 0.1.1-1
- Unbind GL_ARRAY_BUFFER before drawing the drop-simulation room, so its
  client-side vertex arrays cannot be read as offsets into whatever buffer
  Qt Quick left bound. Undefined behaviour that happened to work on Adreno.

* Mon Aug 10 2026 harbour-3dmviewer contributors 0.1.0-1
- First release: Rhino .3dm viewing, orbit/pan/zoom, per-layer visibility.
