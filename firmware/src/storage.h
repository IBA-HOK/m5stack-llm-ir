// LittleFS persistence for the IR database (spec section 7).
//
// The database is a single JSON document at /ir_db.json:
//
//   {
//     "schema_version": 1,
//     "devices": {
//       "living_tv": {
//         "label": "リビングのテレビ",
//         "actions": {
//           "power": {"format": "protocol", "protocol": "NEC",
//                     "code": "0x20DF10EF", "bits": 32}
//         }
//       }
//     }
//   }
//
// Writes are atomic: /ir_db.tmp is written and flushed first, then renamed
// over /ir_db.json (the previous file is briefly kept as /ir_db.bak so a
// failed rename can be rolled back).
#pragma once

#include <Arduino.h>

#include <map>

#include "config.h"
#include "state.h"

namespace storage {

// Mounts LittleFS (formatting it if needed).
bool begin();

// Reads /ir_db.json into `devices`. A missing file is not an error. A corrupt
// file is renamed to /ir_db.corrupt.json and reported through `err`.
bool load(std::map<String, IrDevice>* devices, String* err);

// Atomically writes the database.
bool save(const std::map<String, IrDevice>& devices, String* err);

// Reports LittleFS usage in bytes.
void stats(size_t* totalBytes, size_t* usedBytes);

}  // namespace storage
