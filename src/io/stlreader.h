/*
  harbour-3dmviewer — stlreader.h
  Copyright (C) 2026  JimKnopfIoT — GPLv3 or later.

  Reads STL, both the binary and the ASCII flavour, into a Scene.

  STL IS A TRIANGLE SOUP AND NOTHING ELSE
  ---------------------------------------
  No layers, no colours, no units, no topology: just facets, each carrying its
  three corners and a normal that the writer may or may not have bothered to
  compute. So the whole file becomes one Scene layer, and a normal that is
  missing or degenerate is replaced by the one the triangle itself implies.

  TELLING THE TWO FLAVOURS APART
  ------------------------------
  The obvious test — "does it start with the word solid" — is wrong, because
  binary writers are free to put anything in their 80-byte header and several
  put the model name there, starting with "solid". The reliable test is
  arithmetic: a binary file is exactly 84 + 50 * facetCount bytes long. That is
  what decides here, with the keyword only used when the size test cannot.
*/
#ifndef STLREADER_H
#define STLREADER_H

#include <QString>

#include "model/scene.h"

class StlReader
{
public:
    static ScenePtr read(const QString &path,
                         QString *errorOut,
                         void (*progress)(void *, float) = nullptr,
                         void *progressCtx = nullptr);
};

#endif // STLREADER_H
