# openNURBS — a submodule, not a copy

`opennurbs/` is the **openNURBS Toolkit by Robert McNeel & Associates**, pulled
straight from their own repository at <https://github.com/mcneel/opennurbs> and
pinned to one exact commit. A fresh clone starts out empty:

```sh
git submodule update --init
```

Nothing of McNeel's is stored in this repository. What is recorded is a URL and
a commit hash, so the code you build is the code they published, fetched from
them.

## Why a submodule rather than a bundled copy

Their `LICENSE` file is a warranty disclaimer plus the zlib licence covering the
**bundled zlib subset**; it carries no permission grant for openNURBS itself,
and GitHub's own licence detection reports `NOASSERTION` for the repository.
McNeel state elsewhere that the toolkit may be used and redistributed freely,
including commercially, and that "neither copyright nor copyleft restrictions
apply" —
<https://developer.rhino3d.com/guides/opennurbs/what-is-opennurbs/> — so a
bundled copy would very probably have been fine.

"Very probably fine" is a poor reason to redistribute several hundred files
somebody else wrote. Referencing them costs a larger first clone and nothing
else, and it sidesteps the question instead of betting on the answer.

## The build does not use all of it

`lib.pro`, beside this file, is **ours** — a qmake project compiling the same
set of sources upstream's `CMakeLists.txt` does. It excludes five files upstream
also leaves out (the legacy OpenGL helper, the MSVC precompiled-header stub, two
codepage tables and the obsolete userdata reader), and it never enables
`OPENNURBS_FREETYPE_SUPPORT`, which upstream leaves off for Linux — this
application does not rasterise annotation text.

The submodule itself is untouched: no patch, no fork, no modified file.

## Trademarks

This application is not affiliated with or endorsed by Robert McNeel &
Associates. "Rhinoceros", "Rhino3D" and "openNURBS" are their trademarks, used
here only to name their software accurately.
