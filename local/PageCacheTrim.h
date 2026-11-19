/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Walking a whole document renders every page once, and every render leaves a
// cached display list behind. A display list keeps its page's images alive, and
// the image store can't evict referenced entries, so measuring a scanned book
// cost ~20 MB per page (5 GB and climbing on a 660-page volume of 3700 pt
// pages). These passes hand the pages they are done with back.
struct EngineBase;
void EngineMupdfTrimPageCaches(EngineBase* engine, int keepPageNo);
