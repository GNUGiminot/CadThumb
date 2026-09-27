# Third-party software

Released binaries of CadThumb (`CadThumb.exe`, `CadThumbSetup.exe`) statically link the libraries below.
The source code of CadThumb is published under the MIT license (see [LICENSE](LICENSE)).

## Open CASCADE Technology (OCCT) 8.0.0

* https://github.com/Open-Cascade-SAS/OCCT
* License: GNU Lesser General Public License v2.1 with the Open CASCADE exception
  (`LICENSE_LGPL_21.txt`, `OCCT_LGPL_EXCEPTION.txt` in the OCCT source tree).
* Used by: `CadThumb.exe` (STEP import, tessellation).
* Relinking: the complete source code of CadThumb and the build scripts (`build.ps1`, vcpkg triplet in
  `triplets/`) are in this repository, so the program can be rebuilt against a modified OCCT. To link OCCT
  dynamically instead, build with the vcpkg triplet `x64-windows` and ship the OCCT DLLs next to `CadThumb.exe`.

## miniz

* https://github.com/richgel999/miniz
* License: MIT
* Used by: `CadThumbShell.dll`, `CadThumb.exe` (reading 3MF packages).

## pugixml

* https://pugixml.org
* License: MIT — Copyright (c) Arseny Kapoulkine
* Used by: `CadThumb.exe` (parsing 3MF model XML).
