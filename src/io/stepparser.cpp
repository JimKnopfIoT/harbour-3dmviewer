/*
  harbour-3dmviewer — stepparser.cpp
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.
*/
#include "io/stepparser.h"

#include <QObject>
#include <QtGlobal>

#include <algorithm>

bool StepValue::isEnum(const char *name) const
{
    return kind == Enum && text == QLatin1String(name);
}

const StepRecord *StepRecord::part(const char *name) const
{
    for (int i = 0; i < parts.size(); ++i)
        if (parts.at(i).type == QLatin1String(name))
            return &parts.at(i);
    return nullptr;
}

namespace {

/*
  A scanner over the file with the comments already removed.

  Comments are stripped up front rather than skipped during parsing because
  Part 21 allows a slash-star comment anywhere at all, including between an
  entity's name and its opening bracket. Handling that at every call site
  would mean a comment check in a dozen places; removing them once costs one
  pass.

  Strings are the reason this cannot be a plain search-and-replace: an
  apostrophe inside a string may be followed by anything, and a comment
  opener inside a string is text, not a comment.
*/
QByteArray stripComments(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());

    int i = 0;
    const int n = in.size();
    while (i < n) {
        const char c = in.at(i);

        if (c == '\'') {
            // Copy the string whole. Inside it, '' is an escaped quote and
            // does NOT end the string.
            out.append(c);
            ++i;
            while (i < n) {
                if (in.at(i) == '\'') {
                    if (i + 1 < n && in.at(i + 1) == '\'') {
                        out.append("''", 2);
                        i += 2;
                        continue;
                    }
                    out.append('\'');
                    ++i;
                    break;
                }
                out.append(in.at(i));
                ++i;
            }
            continue;
        }

        if (c == '/' && i + 1 < n && in.at(i + 1) == '*') {
            i += 2;
            while (i + 1 < n && !(in.at(i) == '*' && in.at(i + 1) == '/'))
                ++i;
            i += 2;
            // A comment separates tokens, so it must not glue them together.
            out.append(' ');
            continue;
        }

        out.append(c);
        ++i;
    }
    return out;
}

class Scanner
{
public:
    explicit Scanner(const QByteArray &d) : m_d(d), m_i(0), m_n(d.size()) {}

    void skipSpace()
    {
        while (m_i < m_n) {
            const char c = m_d.at(m_i);
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++m_i;
            else
                break;
        }
    }

    bool atEnd() { skipSpace(); return m_i >= m_n; }
    char peek() { skipSpace(); return m_i < m_n ? m_d.at(m_i) : '\0'; }
    void take() { ++m_i; }
    int pos() const { return m_i; }
    void seek(int p) { m_i = p; }

    // A bare identifier: entity names, and the keywords of the header.
    QString readName()
    {
        skipSpace();
        const int start = m_i;
        while (m_i < m_n) {
            const char c = m_d.at(m_i);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-')
                ++m_i;
            else
                break;
        }
        return QString::fromLatin1(m_d.constData() + start, m_i - start).toUpper();
    }

    StepValue readValue()
    {
        StepValue v;
        skipSpace();
        if (m_i >= m_n)
            return v;

        const char c = m_d.at(m_i);

        if (c == '$') { ++m_i; v.kind = StepValue::Missing; return v; }
        if (c == '*') { ++m_i; v.kind = StepValue::Derived; return v; }

        if (c == '#') {
            ++m_i;
            v.kind = StepValue::Ref;
            v.ref = readInt();
            return v;
        }

        if (c == '\'') {
            ++m_i;
            v.kind = StepValue::Text;
            QByteArray s;
            while (m_i < m_n) {
                if (m_d.at(m_i) == '\'') {
                    if (m_i + 1 < m_n && m_d.at(m_i + 1) == '\'') {
                        s.append('\'');
                        m_i += 2;
                        continue;
                    }
                    ++m_i;
                    break;
                }
                s.append(m_d.at(m_i));
                ++m_i;
            }
            // Part 21 escapes non-ASCII as \X2\00E4\X0\ and friends. Decoding
            // that fully is a job of its own; the names we show come out
            // readable enough without it, and nothing here depends on them.
            v.text = QString::fromUtf8(s);
            return v;
        }

        if (c == '.') {
            ++m_i;
            v.kind = StepValue::Enum;
            const int start = m_i;
            while (m_i < m_n && m_d.at(m_i) != '.')
                ++m_i;
            v.text = QString::fromLatin1(m_d.constData() + start, m_i - start);
            if (m_i < m_n)
                ++m_i;   // closing dot
            return v;
        }

        if (c == '(') {
            ++m_i;
            v.kind = StepValue::List;
            for (;;) {
                skipSpace();
                if (m_i >= m_n)
                    break;
                if (m_d.at(m_i) == ')') { ++m_i; break; }
                if (m_d.at(m_i) == ',') { ++m_i; continue; }
                v.items.append(readValue());
            }
            return v;
        }

        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            v.kind = StepValue::Number;
            v.number = readDouble();
            return v;
        }

        // A typed value inside an argument list, e.g. PARAMETER_VALUE(0.0) or
        // the units in the header. Kept as a one-item list under the name, so
        // callers can look past the wrapper without special cases.
        const QString name = readName();
        if (!name.isEmpty() && peek() == '(') {
            take();
            v.kind = StepValue::List;
            v.text = name;
            for (;;) {
                skipSpace();
                if (m_i >= m_n)
                    break;
                if (m_d.at(m_i) == ')') { ++m_i; break; }
                if (m_d.at(m_i) == ',') { ++m_i; continue; }
                v.items.append(readValue());
            }
            return v;
        }

        // Nothing recognisable: step over one character so the parser cannot
        // spin here on a malformed file.
        if (name.isEmpty())
            ++m_i;
        return v;
    }

    int readInt()
    {
        skipSpace();
        int sign = 1;
        if (m_i < m_n && (m_d.at(m_i) == '-' || m_d.at(m_i) == '+')) {
            if (m_d.at(m_i) == '-')
                sign = -1;
            ++m_i;
        }
        int v = 0;
        while (m_i < m_n && m_d.at(m_i) >= '0' && m_d.at(m_i) <= '9') {
            v = v * 10 + (m_d.at(m_i) - '0');
            ++m_i;
        }
        return v * sign;
    }

    double readDouble()
    {
        skipSpace();
        const int start = m_i;
        if (m_i < m_n && (m_d.at(m_i) == '-' || m_d.at(m_i) == '+'))
            ++m_i;
        while (m_i < m_n) {
            const char c = m_d.at(m_i);
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E')
                ++m_i;
            else if ((c == '-' || c == '+')
                     && (m_d.at(m_i - 1) == 'e' || m_d.at(m_i - 1) == 'E'))
                ++m_i;   // exponent sign
            else
                break;
        }
        return QByteArray(m_d.constData() + start, m_i - start).toDouble();
    }

private:
    const QByteArray &m_d;
    int m_i;
    int m_n;
};

// Reads `NAME(args)` into a record. The scanner must be sitting on the name.
StepRecord readSimpleRecord(Scanner *s)
{
    StepRecord r;
    r.type = s->readName();
    if (s->peek() == '(') {
        s->take();
        for (;;) {
            const char c = s->peek();
            if (c == '\0')
                break;
            if (c == ')') { s->take(); break; }
            if (c == ',') { s->take(); continue; }
            r.args.append(s->readValue());
        }
    }
    return r;
}

} // namespace

bool StepFile::parse(const QByteArray &raw, QString *errorOut)
{
    if (!raw.startsWith("ISO-10303-21")) {
        // Some writers put a byte-order mark or blank lines first, so look a
        // little further before giving up.
        if (!raw.left(256).contains("ISO-10303-21")) {
            if (errorOut)
                *errorOut = QObject::tr("Not a STEP (ISO 10303-21) file.");
            return false;
        }
    }

    const QByteArray data = stripComments(raw);
    Scanner s(data);

    bool inData = false;

    while (!s.atEnd()) {
        const char c = s.peek();

        if (c == ';') {           // stray terminator
            s.take();
            continue;
        }

        if (c == '#') {
            s.take();
            const int id = s.readInt();
            if (s.peek() != '=') {
                // Not a definition; skip to the end of the statement.
                while (!s.atEnd() && s.peek() != ';')
                    s.take();
                continue;
            }
            s.take();   // '='

            StepRecord rec;
            if (s.peek() == '(') {
                // Complex instance: a bracketed run of simple records.
                s.take();
                for (;;) {
                    const char k = s.peek();
                    if (k == '\0' || k == ')') { if (k == ')') s.take(); break; }
                    if (k == ',') { s.take(); continue; }
                    rec.parts.append(readSimpleRecord(&s));
                }
                if (!rec.parts.isEmpty()) {
                    rec.type = rec.parts.first().type;
                    rec.args = rec.parts.first().args;
                }
            } else {
                rec = readSimpleRecord(&s);
            }

            if (id > 0 && inData)
                m_records.insert(id, rec);

            while (!s.atEnd() && s.peek() != ';')
                s.take();
            if (!s.atEnd())
                s.take();
            continue;
        }

        // A bare keyword: HEADER, DATA, ENDSEC, or a header record.
        const int before = s.pos();
        const QString word = s.readName();
        if (word.isEmpty()) {
            if (s.pos() == before)
                s.take();   // never stall
            continue;
        }

        if (word == QLatin1String("DATA")) {
            inData = true;
        } else if (word == QLatin1String("ENDSEC")) {
            inData = false;
        } else if (word == QLatin1String("FILE_NAME") && s.peek() == '(') {
            s.seek(before);
            const StepRecord r = readSimpleRecord(&s);
            if (r.args.size() > 0 && r.args.at(0).kind == StepValue::Text)
                m_fileName = r.args.at(0).text;
            // originating_system is the sixth field, and it is the one worth
            // showing: it names the CAD system rather than the translator.
            if (r.args.size() > 5 && r.args.at(5).kind == StepValue::Text)
                m_originatingSystem = r.args.at(5).text;
        }

        while (!s.atEnd() && s.peek() != ';')
            s.take();
        if (!s.atEnd())
            s.take();
    }

    if (m_records.isEmpty()) {
        if (errorOut)
            *errorOut = QObject::tr("The STEP file contains no data section.");
        return false;
    }
    return true;
}

const StepRecord *StepFile::record(int id) const
{
    const QHash<int, StepRecord>::const_iterator it = m_records.constFind(id);
    return it == m_records.constEnd() ? nullptr : &it.value();
}

QVector<int> StepFile::idsOfType(const QString &type) const
{
    QVector<int> ids;
    for (QHash<int, StepRecord>::const_iterator it = m_records.constBegin();
         it != m_records.constEnd(); ++it) {
        if (it.value().type == type)
            ids.append(it.key());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}
