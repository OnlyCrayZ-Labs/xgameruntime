<!-- MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md. -->
<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->

# Local XGameSave provider

XGameSave is implemented as a local provider. The Xbox cloud protocol behind
it (Connected Storage) is not publicly documented, so nothing is uploaded or
downloaded: saves do not roam between devices, and saves made on other
devices (for example on Windows) are not visible. Everything else follows the
GDK contract.

Storage: `%LOCALAPPDATA%\Xodus\GameSave\<XUID>\<SCID>\` inside the Wine prefix,
one `.xgs` file per container (name hash plus a slot number; the file stores
the exact name). Format: "XGS1", u32 version 1, length-prefixed NUL-terminated
name and display name, i64 last modified (Unix seconds), u32 blob count, per
blob a length-prefixed name and u32 size, then the blob data in order.

- Updates are atomic per container: the new container is written to
  `<file>.tmp`, flushed and renamed over the old file. A failing operation
  (missing blob to delete, quota, size) leaves the container unchanged.
- Limits: container names 1..256 of letters, digits, `_` and `/`
  (E_GS_INVALID_CONTAINER_NAME); blob names 1..256 printable characters;
  writes per update at most 16 MB (E_GS_UPDATE_TOO_BIG); 4096 blobs per
  container; 256 MB of blob data per user and SCID (E_GS_QUOTA_EXCEEDED,
  GetRemainingQuota).
- `needsSync` is always FALSE: GDK sets it only for syncOnDemand when local
  and server data differ, and there is no server copy here. syncOnDemand is
  accepted and has no further effect.
- GetContainerInfo of an absent container succeeds without a callback;
  deleting an absent container succeeds; reading named blobs that do not exist
  fails with E_GS_BLOB_NOT_FOUND; a never-written container reads as empty.
- ReadBlobData buffer layout follows the GDK samples' sizing: the
  XGameSaveBlob array followed by each blob's data (sum of sizeof(XGameSaveBlob)
  plus blob sizes). Blob names returned in it are owned by the container handle
  and stay valid until XGameSaveCloseContainer. Too small: E_GS_PROVIDED_BUFFER_TOO_SMALL.
- An update can be submitted once; afterwards SubmitUpdate and further writes
  return E_INVALIDARG. Do not close an update while its asynchronous submit runs.
- Async forms (InitializeProvider, GetRemainingQuota, DeleteContainer,
  ReadBlobData, SubmitUpdate) run the same work on the async queue.
- XGameSaveFiles* remain unimplemented (E_NOTIMPL); GDK forbids mixing them
  with XGameSave anyway.

Tests: tests/xbl.c `gamesave()` (run through tests/xbl-fixture.py, which
provides a signed-in synthetic user).
