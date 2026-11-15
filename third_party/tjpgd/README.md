# TJpgDec R0.03

Tiny JPEG decompressor by ChaN, http://elm-chan.org/fsw/tjpgd/
(tjpgd3.zip, sha256 052fe3efbc9a8be29f31597ad009c5b51a4f6905878eb28569e0ab3d46d0c013).
License: see the header of tjpgd.c (free, no restriction on use).

Local change: `tjpgdcnf.h` set to RGB565 output (`JD_FORMAT 1`) and 32-bit
optimization (`JD_FASTDECODE 1`). Used by `disp.jpeg()` (app/src/jpeg.c),
in the firmware and the simulator alike.
