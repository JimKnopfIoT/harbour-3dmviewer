/*
  harbour-3dmviewer — bgcodecodecs.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  The two compression layers a PrusaSlicer .bgcode file puts between us and the
  G-code text. Both are written from the format description rather than taken
  from an existing implementation, so nothing here inherits a licence that
  would travel further than this program's own.

  WHY NOT USE libbgcode
  ---------------------
  Prusa's own reader is AGPL. Linking it into a viewer would push the whole app
  under the AGPL, and the format is small enough that reading it directly is
  the cheaper answer: a bit-level decoder of about a hundred lines and a nibble
  unpacker of about the same.
*/
#ifndef BGCODECODECS_H
#define BGCODECODECS_H

#include <QByteArray>

namespace BgCodeCodecs {

/*
  Heatshrink — an LZSS variant with no entropy coding, decoded from an MSB-first
  bitstream:

      1 <8 bits>                       a literal byte
      0 <windowBits> <lookaheadBits>   copy (count+1) bytes from (offset+1) back

  The back-reference reads from what has already been produced, so an offset
  smaller than the count is legal and repeats a pattern — the copy therefore
  has to go one byte at a time and cannot become a memcpy.

  `expectedSize` comes from the block header and is the only reliable end
  marker: the encoder pads the last byte with zero bits, which would otherwise
  decode as one more spurious back-reference.
*/
QByteArray heatshrinkDecode(const QByteArray &in,
                            int windowBits,
                            int lookaheadBits,
                            int expectedSize);

/*
  MeatPack — packs two characters into one byte whenever both are among the
  fifteen that make up almost all of a G-code file:

      0-9  .  E  \n  G  X          and 0xF as an escape

  A nibble of 0xF means "this character is not in the table"; the full byte
  follows the packed byte, and if both nibbles escape then two bytes follow, in
  order. The LOW nibble is the first character.

  Two consecutive 0xFF bytes introduce a command rather than data:

      0xFB enable packing      0xFA disable packing (raw bytes follow)
      0xF7 no-spaces mode      0xF6 normal mode

  No-spaces mode is what PrusaSlicer actually writes, and it matters: it
  replaces the space in slot 11 with 'E'. Decode the same stream with a space
  there and every extrusion distance silently loses its E — the file still
  looks like plausible G-code, which is exactly what makes the mistake
  expensive. Spaces are then written as escaped bytes when they do occur.
*/
QByteArray meatPackDecode(const QByteArray &in);

} // namespace BgCodeCodecs

#endif // BGCODECODECS_H
