// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Internal async provider implementation. LGPL-2.1-or-later. */
HRESULT async_begin(XAsyncBlock *, void *, const void *, XAsyncProvider *);
HRESULT async_schedule(XAsyncBlock *, UINT32);
void async_complete(XAsyncBlock *, HRESULT, SIZE_T);
HRESULT async_status(XAsyncBlock *, BOOLEAN);
HRESULT async_size(XAsyncBlock *, SIZE_T *);
HRESULT async_result(XAsyncBlock *, const void *, SIZE_T, void *, SIZE_T *);
void async_cancel(XAsyncBlock *);
HRESULT async_begin_owned(XAsyncBlock *, void *, const void *, XAsyncProvider *, void (*)(void *));
