// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// What survives a DSi session. The dump is never written: session writes live in NandImage's
// memory and are carried out as files, put back before the next boot.
//
//   title saves   /title/00030004/<id>/data/{public,private,banner}.sav -> <saves>/<GAMECODE>.
//                 pub/.prv/.bnr (melonDS TitleManager export format)
//   system files  any other file on partition 0 the session wrote -> one sidecar beside the dump
//   photos        files the session wrote on the photo partition -> plain files under a folder
//
// Left out: /tmp and /import (scratch space), and a virtual install's title contents/tickets.
#pragma once
#include "core/types.h"

#include <string>
#include <vector>

namespace ds::io {

class NandImage;

struct NandPersistPaths {
  std::string saves_dir;     // <GAMECODE>.pub/.prv/.bnr
  std::string sidecar;       // the system files
  std::string photos_dir;    // the photo partition's files

  // Defaults: saves in `saves_dir` if given, else beside the dump as "<nand>.ovr"/"<nand>.photos".
  static NandPersistPaths beside(const std::string& nand_path, const std::string& saves_dir = {});
};

struct NandPersistReport {
  int saves = 0, system_files = 0, photos = 0;   // files moved
  std::vector<std::string> notes;                // skipped files and why, one line each
};

// Puts saved files into the session. Call after the NAND is loaded, before the boot that reads
// it. `bios7i` may be null (tickets untouched here).
NandPersistReport nand_import(NandImage& nand, const u8* bios7i, const NandPersistPaths& paths);

// Carries out what the session changed. Cheap when nothing was written: only files whose
// sectors the session wrote are read, and a host file is only rewritten if it differs.
NandPersistReport nand_export(NandImage& nand, const u8* bios7i, const NandPersistPaths& paths);

}  // namespace ds::io
