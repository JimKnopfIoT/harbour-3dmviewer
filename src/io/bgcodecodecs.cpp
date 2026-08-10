/*
  harbour-3dmviewer — bgcodecodecs.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/bgcodecodecs.h"

namespace {

// Reads an MSB-first bitstream. Returns -1 once the input is exhausted, which
// the callers treat as "stop", never as a value.
class BitReader
{
public:
    explicit BitReader(const QByteArray &data)
        : m_data(data), m_pos(0), m_bits(qint64(data.size()) * 8) {}

    int read(int n)
    {
        if (m_pos + n > m_bits)
            return -1;
        int v = 0;
        for (int i = 0; i < n; ++i) {
            const uchar byte = uchar(m_data.at(int(m_pos >> 3)));
            const int bit = (byte >> (7 - int(m_pos & 7))) & 1;
            v = (v << 1) | bit;
            ++m_pos;
        }
        return v;
    }

private:
    const QByteArray &m_data;
    qint64 m_pos;
    qint64 m_bits;
};

// MeatPack's fifteen packed characters. Slot 11 depends on the mode the stream
// selected; see the header for why getting it wrong is so easy to miss.
const char kPackedNormal[15]   = { '0','1','2','3','4','5','6','7','8','9','.',' ','\n','G','X' };
const char kPackedNoSpaces[15] = { '0','1','2','3','4','5','6','7','8','9','.','E','\n','G','X' };

} // namespace

namespace BgCodeCodecs {

QByteArray heatshrinkDecode(const QByteArray &in,
                            int windowBits,
                            int lookaheadBits,
                            int expectedSize)
{
    QByteArray out;
    if (expectedSize <= 0 || windowBits <= 0 || lookaheadBits <= 0)
        return out;
    out.reserve(expectedSize);

    BitReader bits(in);

    while (out.size() < expectedSize) {
        const int tag = bits.read(1);
        if (tag < 0)
            break;

        if (tag == 1) {
            const int b = bits.read(8);
            if (b < 0)
                break;
            out.append(char(uchar(b)));
            continue;
        }

        const int rawOffset = bits.read(windowBits);
        const int rawCount = bits.read(lookaheadBits);
        if (rawOffset < 0 || rawCount < 0)
            break;

        const int offset = rawOffset + 1;
        const int count = rawCount + 1;
        // A reference reaching further back than we have produced means the
        // stream is damaged. Returning what we have beats reading out of
        // bounds, and the caller notices the short result.
        if (offset > out.size())
            break;

        for (int i = 0; i < count && out.size() < expectedSize; ++i)
            out.append(out.at(out.size() - offset));
    }

    return out;
}

QByteArray meatPackDecode(const QByteArray &in)
{
    QByteArray out;
    // Packed G-code roughly halves; reserving twice the input saves the
    // reallocations that would otherwise dominate a 40 kB block.
    out.reserve(in.size() * 2);

    const char *table = kPackedNormal;
    bool packing = true;

    int i = 0;
    const int n = in.size();
    while (i < n) {
        const uchar b = uchar(in.at(i));

        // 0xFF twice = a command. A single 0xFF is ordinary data: both nibbles
        // escaped, with the two literal bytes following.
        if (b == 0xFF && i + 1 < n && uchar(in.at(i + 1)) == 0xFF) {
            const uchar cmd = (i + 2 < n) ? uchar(in.at(i + 2)) : 0;
            i += 3;
            switch (cmd) {
            case 0xFB: packing = true; break;
            case 0xFA: packing = false; break;
            case 0xF7: table = kPackedNoSpaces; break;
            case 0xF6: table = kPackedNormal; break;
            default: break;   // reset/query concern the printer, not a reader
            }
            continue;
        }

        if (!packing) {
            out.append(char(b));
            ++i;
            continue;
        }

        const int lo = b & 0x0F;
        const int hi = (b >> 4) & 0x0F;
        ++i;

        // Both nibbles are resolved before either literal is consumed, because
        // the escaped bytes follow the packed byte in nibble order.
        if (lo == 0x0F) {
            if (i >= n)
                break;
            out.append(in.at(i++));
        } else {
            out.append(table[lo]);
        }

        if (hi == 0x0F) {
            if (i >= n)
                break;
            out.append(in.at(i++));
        } else {
            out.append(table[hi]);
        }
    }

    return out;
}

} // namespace BgCodeCodecs
