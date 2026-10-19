# Tag export / import: design handoff

Status: design agreed in discussion, nothing implemented. This document is the
brief for whoever implements it. It records the decisions, the codebase facts
they rest on, the schema changes, and a phasing that keeps each step shippable.

## Decisions taken by default

These were left open and the recommended option was taken. Override here before
starting.

| Decision | Default taken | Alternative |
|---|---|---|
| Automatic sidecar writing | Off by default. On-demand export always available. A setting enables write-behind. | Default on |
| `\|` in tag and category names | Forbidden in the editor. Migration replaces existing `\|` with `/`. | Escape on export instead |
| Embedded XMP keywords on import | Read in v1. Sidecar wins over embedded when both exist. | Defer to later |
| Favorites ↔ `xmp:Rating` | No mapping. Favorite lives only in the omapic namespace. | Export favorite as Rating 5 |

## Goals

1. Export tags so other apps (darktable, digiKam, Lightroom, XnView, Immich, …)
   can read them.
2. Import tags written by those apps.
3. Move a whole library's tagging, including categories, colors, implications,
   albums and smart albums, to a second omapic library.
4. Make tags survive a file being edited (metadata or pixels) by another tool.
5. Keep search and filter untouched by all of the above.

## Codebase facts the design rests on

- **Tags are content-addressed by the SHA-256 of the whole file.**
  `Database::hashFile` (src/database.cpp) hashes every byte. `content_tags(hash,
  tag_id)` archives tags against that hash so they survive moves, deletes and
  re-imports. `applyContentTags` reattaches them when a file with a known hash is
  imported.
- **Rescan never rehashes a known path.** `LibraryWorker::rescan`
  (src/libraryworker.cpp) only hashes new paths. A file edited in place keeps its
  row, its `photo_tags` and a now-stale hash. Damage is deferred to the next move
  or re-import, when the new hash matches nothing.
- **Search and filter never touch `content_tags`.** Every read query behind the
  gallery, tag list and category list uses `photos` and `photo_tags` only. The
  archive is written on import, tag, untag and implication closure, and read once
  per new file on import. Nothing in this design may add a join to those read
  paths.
- **The scanner already opens every image with exiv2.** `capturedAtEpoch`
  (src/photometadata.cpp) calls `ImageFactory::open` + `readMetadata` on import,
  which parses the XMP packet too. Reading keywords from embedded metadata on
  import is therefore nearly free.
- **Exiv2 write support by format:** JPEG, TIFF, PNG (XMP in iTXt), WebP: yes.
  HEIC/HEIF/AVIF: read only. GIF, BMP, all video: no. This is why sidecars are
  the only universal option and why this design writes nothing into photos.
- **Albums are tags** in a reserved system category (`_system:Albums`);
  Favorites is a system album (`_album:Favorites`). See `systemStoredName` /
  `albumStoredName` in src/database.cpp.
- **Schema is at version 9** (`kSchemaVersion`, src/database.cpp) with an
  additive migration ladder. Non-additive migrations call `backupDatabase` first.
- **The worker thread owns all slow I/O** and the DB is in WAL mode, so worker
  writes never block UI reads.
- **Three paths remove photo rows and all keep the archive:** rescan dropping a
  missing row (libraryworker.cpp), `deletePhotosUnderPath` on folder removal,
  and the in-app move-to-trash (`Library::...` around src/library.cpp:580).
- Settings use `QSettings` under `~/.config/omapic/`.

## Rejected alternatives and why

- **Writing tags into the photo file.** Changes the content hash omapic keys on,
  modifies originals, fails on HEIC/AVIF/GIF/video and read-only mounts, and
  costs a full file rewrite plus rehash per photo. Sidecars cover every format
  and never touch the photo.
- **Metadata-insensitive (payload-only) hash.** Would need per-format parsers
  and a full rehash migration. The mtime-triggered rekey below plus the sidecar
  hash link covers the same cases except duplicate detection between an edited
  copy and an unedited one. Deferred.
- **Normalising the archive to a `contents(id, hash BLOB, …)` table.** Zero
  read-path benefit (the archive is never on the read path), a few MB of disk
  saved, and a large one-way migration touching every hash query and ~30 test
  assertions. Rejected in favour of one additive table.

## Design

### 1. Scanner groundwork: mtime/size, rekey, orphan tracking

Purpose: make tags survive in-place edits without any sidecar, and give the
retention policy something to work with.

- Add `photos.mtime INTEGER` and `photos.size INTEGER` (additive).
- Change `mediascan::collect` to return `QFileInfo` (or path + mtime + size)
  rather than bare paths. The directory walk already stats every file, so this
  makes the per-file stat on rescan free. On NFS/SMB a separate stat pass would
  cost tens of seconds on large libraries; capturing it in the walk avoids that.
- **Rekey on rescan.** For a known path whose mtime or size changed: rehash,
  `INSERT OR IGNORE` the old hash's `content_tags` rows under the new hash,
  update `photos.hash`, then delete the old hash's archive rows **only if no
  other photo row still references the old hash** (protects the untouched twin
  of an edited duplicate). This is the "superseded" case and it ignores the
  retention setting entirely.
- **Orphan tracking.** New additive table:

  ```sql
  CREATE TABLE IF NOT EXISTS orphaned_hashes (
    hash        TEXT PRIMARY KEY,
    orphaned_at INTEGER NOT NULL
  );
  ```

  A row exists only while a hash has archive rows and no photo row. Insert when
  the last referencing row goes away (the three removal paths above; the check
  is `NOT EXISTS (SELECT 1 FROM photos WHERE hash = ?)`, served by
  `idx_photos_hash`). Delete when a photo with that hash is inserted or
  relocated. Never consulted by gallery queries.
- Migration: additive (`CREATE TABLE IF NOT EXISTS` + `ALTER TABLE ADD COLUMN`).
  Existing orphans at migration time get `orphaned_at = now` (clock starts
  conservatively). Seed them in the worker's `runMigration` with one query:
  `INSERT OR IGNORE INTO orphaned_hashes SELECT DISTINCT hash, ? FROM
  content_tags WHERE hash NOT IN (SELECT hash FROM photos WHERE hash IS NOT NULL)`.

### 2. Sidecar format

- **Container:** XMP (RDF/XML). Read and write through exiv2's `.xmp` sidecar
  image type (`ImageFactory::open("x.xmp")`, `readMetadata`, `writeMetadata`).
  Do not hand-build XML.
- **Name:** `photo.jpg.xmp` (full name + `.xmp`, as darktable and digiKam
  write). Read `photo.xmp` (Adobe convention) as a fallback. Videos the same:
  `clip.mp4.xmp`. The scanner's glob list only matches media extensions, so
  sidecars are never picked up as photos.
- **Custom namespace:** register once at startup with
  `Exiv2::XmpProperties::registerNs(uri, "omapic")`. Use a URI under something
  the project controls, e.g. the repository URL plus `/xmp/1.0/`.
- **Fields written** (and only these; everything else in the packet is
  preserved verbatim):

  | Key | Type | Content |
  |---|---|---|
  | `Xmp.dc.subject` | bag | Flat tag names. Excludes albums and Favorites. |
  | `Xmp.lr.hierarchicalSubject` | bag | `Category\|Tag`; uncategorised tags as bare `Tag`. |
  | `Xmp.omapic.contentHash` | text | `sha256:<hex>` of the file at write time. |
  | `Xmp.omapic.albums` | bag | Album display names (system prefix stripped). |
  | `Xmp.omapic.favorite` | bool | `True` when in Favorites. |
  | `Xmp.xmp.MetadataDate` | date | When omapic last wrote this sidecar. |

- **Fields read** on import, in addition to the above: `Xmp.digiKam.TagsList`
  (`Category/Tag`), `Xmp.MicrosoftPhoto.LastKeywordXMP`, and for embedded
  metadata `Iptc.Application2.Keywords`. Deeper hierarchies than two levels are
  flattened: first component becomes the category, the remainder joined with
  `/` becomes the tag name.
- **Why no second tag list in the omapic namespace:** it would be two sources
  of truth in one file. The only loss in the standard fields is a `|` in a tag
  name, handled by forbidding the character (section 7).
- Category colors, implications and smart albums are library-level and do not
  go in sidecars. They go in the manifest (section 5).

### 3. Sidecar and embedded metadata read

Where it hooks in: `importDirectory` and the new-path branch of `rescan`, plus a
new per-known-path check in `rescan`.

- **New file (new path, unknown hash):** tags = union(archive by hash, sidecar,
  embedded keywords). Sidecar wins over embedded for the same field; union
  across fields. Everything applies through the existing tag APIs so
  implications and `content_tags` are materialised as usual.
- **Sidecar hash link:** if `omapic:contentHash` differs from the file's actual
  hash, the recorded hash is *superseded* by the actual one: copy its archive
  rows forward and prune it if unreferenced, exactly as the rescan rekey does.
  This closes the edit-plus-move-between-rescans gap and doubles as a check
  that the sidecar belongs to this file.
- **Known file whose sidecar changed:** on rescan, if the sidecar's mtime is
  newer than `photos.sidecar_written_at` (or that column is NULL and a sidecar
  exists), another app wrote it. Union its keyword fields into the DB. This is
  what makes darktable → omapic tag sync work for files already in the library.
- **Imports are additive.** Never remove a tag because a sidecar lacks it.
  Deletion propagation can be a later setting.
- **Tag matching:** by name under the existing `UNIQUE … COLLATE NOCASE`.
  Accent-insensitive merging is a separate policy choice; do not add it here.
- **Category from import:** created if missing, with the default color slot.
- Ignore `omapic:*` keys in a sidecar whose `contentHash` is absent or matches
  nothing we know; still read the standard keyword fields from it.

### 4. Sidecar write

- Add `photos.sidecar_written_at INTEGER` (additive) and a queue table:

  ```sql
  CREATE TABLE IF NOT EXISTS sidecar_dirty (
    photo_id INTEGER PRIMARY KEY REFERENCES photos(id) ON DELETE CASCADE
  );
  ```

  Every tag/untag/album/favorite change inserts the photo ids (`INSERT OR
  IGNORE`). Cheap, persistent, and coalesces repeated changes to one write.
- **Flush** runs on the worker thread. Triggered by: the on-demand export action
  (selection, folder, or whole library; it enqueues the chosen photos then
  flushes), and, when the "write sidecars automatically" setting is on, a
  debounced timer after tag changes and at the end of rescan.
- **Per-photo flush procedure:**
  1. If the sidecar exists and its mtime is newer than `sidecar_written_at`,
     run the section 3 import first. Otherwise a keyword another app added
     since the last rescan would be dropped by the replace below.
  2. Load the existing packet (if any). Replace only the keys in the section 2
     table. Leave every other key untouched; darktable stores its whole edit
     history in this file and truncating it destroys edits.
  3. If the photo has no tags, albums or favorite: remove omapic's keys. Delete
     the file only if omapic created it (no other keys remain). Otherwise leave
     it.
  4. Write atomically (temp file in the same directory, then rename).
  5. Set `sidecar_written_at` and delete the `sidecar_dirty` row.
- A write failure (read-only mount, permission) leaves the dirty row in place
  and is reported once per flush, not per photo.

### 5. Library manifest (omapic ↔ omapic)

A single JSON file, exported for the whole library, importable into another
library. Keyed by content hash, so it works for every format and writes no
files next to photos.

```json
{
  "omapic_manifest": 1,
  "exported_at": "2026-10-19T12:00:00Z",
  "categories": [ { "name": "People", "color_slot": 3 } ],
  "tags": [ { "name": "Alice", "category": "People" } ],
  "implications": [ { "if": "beach", "then": "vacation" } ],
  "albums": [ "Summer 2026" ],
  "smart_albums": [ { "name": "Trips", "match_all": true, "tags": ["vacation"] } ],
  "content": [
    { "hash": "sha256:…", "tags": ["Alice", "beach"], "albums": ["Summer 2026"], "favorite": true }
  ]
}
```

- `content` is derived from `content_tags` (live and orphaned hashes alike),
  so it is also the backup a user can take before aggressive pruning.
- **Import is additive.** Categories and tags merge by name (NOCASE). On a
  category color conflict the local color wins. Implications and smart albums
  merge by name. `content` rows go into `content_tags`; then
  `applyContentTags` reattaches them to any live photo with a matching hash.
  Photos imported later pick them up through the normal path.
- System names: albums are exported by display name and re-created under the
  system Albums category on import. Favorites is a flag, not an album name.

### 6. Retention

- Setting: "Remember tags for removed files": forever (default) / 1 year /
  6 months / 30 days. Forever is the current promise in the README and must
  stay the default.
- Prune at the end of rescan, on the worker, in one transaction:
  `DELETE FROM content_tags WHERE hash IN (SELECT hash FROM orphaned_hashes
  WHERE orphaned_at < ?)` then the same for `orphaned_hashes`. Superseded
  hashes were already pruned by the rekey and are unaffected by this setting.
- Maintenance action "Forget tags for files removed before …" available under
  any policy, with a count preview before committing. Offer to write the
  manifest first.
- Library stats line: "tags remembered for N removed files".
- Help text should say the byte-identical caveat plainly: a re-download only
  recovers tags if the bytes are identical; most social platforms re-encode.
- Forgetting never deletes tag definitions, categories, or implications.

### 7. Tag name validation

- Reject `|` in tag and category names in the editor (Lightroom imposes the
  same rule).
- Migration (non-additive, so `backupDatabase` first): replace `|` with `/` in
  existing `tags.name` and `tag_categories.name`, resolving any resulting
  NOCASE collision by merging into the existing tag.

## Schema changes (one bump, 9 → 10)

| Change | Additive? |
|---|---|
| `photos.mtime`, `photos.size`, `photos.sidecar_written_at` | yes |
| `orphaned_hashes` table | yes |
| `sidecar_dirty` table | yes |
| `\|` → `/` rename in tag and category names | **no**, triggers backup |

If section 7 ships separately, the rename can be its own later bump and the
first bump stays fully additive.

## Phasing

Each phase is independently shippable and testable.

1. **Groundwork (section 1).** mtime/size capture in the walk, rekey on
   rescan, `orphaned_hashes`, pruning of superseded hashes. No UI.
2. **Read path (section 3).** Sidecar and embedded keyword import on new files;
   sidecar-changed import on known files; hash link supersession.
3. **Write path (sections 2, 4).** Dirty queue, flush, on-demand export action,
   then the automatic setting.
4. **Manifest (section 5).** Export and import actions.
5. **Retention UI (section 6)** and **name validation (section 7).**

## Tests

Existing suites live under `tests/` (Qt Test, CTest). Add:

- `tst_database`: rekey copies archive rows forward and prunes only when
  unreferenced (duplicate-twin case); orphan row lifecycle across the three
  removal paths and re-import; retention prune respects the cutoff and never
  touches superseded or live hashes.
- `tst_mediascan`: the walk returns mtime and size; a touched file is reported
  changed.
- New `tst_sidecar`: round-trip write → read in a temp dir; write preserves
  foreign keys in an existing packet (seed a packet with a fake
  `darktable:history` key and assert it survives); empty-tag case removes
  omapic keys and deletes only omapic-created files; hash link marks the
  recorded hash superseded; `photo.xmp` fallback is read.
- New `tst_manifest`: export from one in-memory DB, import into a fresh one,
  assert categories, colors, implications, albums, smart albums and
  `content_tags` match; import into a DB with conflicting names merges rather
  than duplicates.

## Verify empirically before relying on it

- Whether darktable preserves the `omapic` namespace when it rewrites a sidecar
  it co-owns. If it drops it, the hash link is lost for darktable users and
  recovery falls back to the rescan rekey, which still works. Test: tag in
  omapic, edit in darktable, check the sidecar.
- Exiv2's sidecar write is atomic on the target filesystem (it should write a
  temp and rename; if not, do it in omapic with `QSaveFile`).
- Exiv2 can set bag and boolean values in a custom namespace without a
  pre-registered property schema (it can for text; confirm for `xmpBag`).

## Performance guardrails

- No new joins or columns in any query under `loadPhotos`, tag list, or
  category list paths.
- All sidecar, hash and prune I/O on the worker thread.
- Per-rescan cost added for a known file: zero extra stats (captured in the
  walk) plus one stat for the sidecar. Full read only when mtime/size changed.
- Per-import cost added for a new file: one sidecar stat and parse; embedded
  keyword read rides the exiv2 open that already happens.
