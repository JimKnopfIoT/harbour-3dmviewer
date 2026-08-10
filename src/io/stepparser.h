/*
  harbour-3dmviewer — stepparser.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  A reader for the ISO 10303-21 exchange structure — the "Part 21" text form
  that every .stp file is written in. This layer knows nothing about geometry:
  it turns the file into numbered records, each with a type name and a list of
  values, and hands that to stepreader.cpp.

  THE FILE IS A FLAT DATABASE, NOT A TREE
  ---------------------------------------
      #99=ADVANCED_FACE('',(#14),#238,.T.);

  Every line is one record with an id. Arguments are literals or references to
  other ids, in any order — a record may well refer forward to one defined
  thousands of lines later. So the file is read once into a map from id to
  record, and only then walked.

  WHAT IS DELIBERATELY NOT SUPPORTED
  ----------------------------------
  Part 21 also allows several data sections, anchors and references between
  files. No CAD system writes those for a single exported solid, and every
  one of them would be dead code here.
*/
#ifndef STEPPARSER_H
#define STEPPARSER_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

// One argument of a record.
struct StepValue
{
    enum Kind {
        Missing,   // $   — the value is absent
        Derived,   // *   — inherited from a supertype
        Ref,       // #123
        Number,
        Text,      // 'a string'
        Enum,      // .T. / .F. / .UNSPECIFIED.
        List       // (...)
    };

    Kind kind = Missing;
    int ref = 0;             // Ref
    double number = 0.0;     // Number
    QString text;            // Text and Enum, without the quotes or dots
    QVector<StepValue> items;   // List

    bool isRef() const { return kind == Ref; }
    bool isList() const { return kind == List; }
    // Enums are compared without their dots: value("T") for .T.
    bool isEnum(const char *name) const;
};

struct StepRecord
{
    QString type;              // "ADVANCED_FACE"
    QVector<StepValue> args;

    /*
      A complex instance — `#5=(A(...) B(...) C(...))` — is how Part 21 writes
      multiple inheritance, and it is the usual shape of a rational B-spline.
      The parts are kept whole here; `type` then holds the first part's name so
      a caller that does not care still sees something sensible.
    */
    QVector<StepRecord> parts;
    bool isComplex() const { return !parts.isEmpty(); }
    // Finds one part of a complex instance by name, or null.
    const StepRecord *part(const char *name) const;
};

class StepFile
{
public:
    // Reads the whole exchange structure. On failure `errorOut` says why.
    bool parse(const QByteArray &data, QString *errorOut);

    const StepRecord *record(int id) const;
    // Every record of one type, in ascending id order, so a redraw of the same
    // file always produces the same geometry order.
    QVector<int> idsOfType(const QString &type) const;

    // FILE_NAME's originating_system, for the "produced by" line.
    QString originatingSystem() const { return m_originatingSystem; }
    QString fileName() const { return m_fileName; }

    int recordCount() const { return m_records.size(); }

private:
    QHash<int, StepRecord> m_records;
    QString m_originatingSystem;
    QString m_fileName;
};

#endif // STEPPARSER_H
