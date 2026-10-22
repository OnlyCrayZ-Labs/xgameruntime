// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGameSave and XGameSaveFiles
 *
 * Copyright 2026 Olivia Ryan
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "private.h"
#include "async_private.h"

struct x_game_save
{
    IXGameSaveImpl3 IXGameSaveImpl3_iface;
    LONG ref;
};

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

static inline struct x_game_save *impl_from_IXGameSaveImpl3( IXGameSaveImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game_save, IXGameSaveImpl3_iface );
}

static HRESULT WINAPI x_game_save_QueryInterface( IXGameSaveImpl3 *iface, REFIID iid, void **out )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown        ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameSaveImpl3 ))
    {
        IXGameSaveImpl_AddRef( *out = &impl->IXGameSaveImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_save_AddRef( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_save_Release( IXGameSaveImpl3 *iface )
{
    struct x_game_save *impl = impl_from_IXGameSaveImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* Local XGameSave provider.
 *
 * Containers are stored in the Wine prefix under
 * %LOCALAPPDATA%\Xodus\GameSave\<XUID>\<SCID>\, one file per container.
 * Every update rewrites the container into a temporary file that atomically
 * replaces the previous one, so an update is applied completely or not at
 * all. Nothing is uploaded: the Xbox cloud (Connected Storage) protocol is not
 * publicly documented, so saves neither roam nor include saves made on other
 * devices. needsSync stays FALSE because there is no server copy to differ
 * from; it does not claim that data was synchronized.
 *
 * Container file: "XGS1", u32 version 1, then u32 length (including NUL) +
 * bytes for the name and the display name, i64 last modified (Unix seconds),
 * u32 blob count, per blob u32 name length (including NUL) + name and u32
 * size, followed by the blob data in the same order. */

#define GS_MAX_UPDATE_SIZE (16 * 1024 * 1024) /* GS_MAX_BLOB_SIZE */
#define GS_QUOTA ((INT64)256 * 1024 * 1024)
#define GS_MAX_CONTAINER_NAME 256
#define GS_MAX_BLOB_NAME 256
#define GS_MAX_BLOBS 4096
#define GS_SLOTS 16
#define GS_MAGIC 0x31534758 /* "XGS1" */

struct gs_blob { const char *name; UINT32 size; const BYTE *data; };
struct gs_image
{
    const char *name, *display;
    INT64 modified;
    UINT32 count;
    struct gs_blob *blobs;
    BYTE *raw;
};

struct gs_provider
{
    struct gs_provider *next;
    LONG refs;
    BOOL closed;
    SRWLOCK io;
    WCHAR root[MAX_PATH];
};
struct gs_container
{
    struct gs_container *next;
    LONG refs;
    BOOL closed;
    struct gs_provider *provider;
    char name[GS_MAX_CONTAINER_NAME + 1];
    /* Names returned by ReadBlobData stay valid until the container closes. */
    SRWLOCK pool_lock;
    char **pool;
    UINT32 pool_count;
};
struct gs_op { char *name; BYTE *data; SIZE_T size; BOOL remove; };
struct gs_update
{
    struct gs_update *next;
    struct gs_container *container;
    char *display;
    struct gs_op *ops;
    UINT32 count, capacity;
    SIZE_T written;
    BOOL submitted;
};

static SRWLOCK gs_lock = SRWLOCK_INIT;
static struct gs_provider *providers;
static struct gs_container *containers;
static struct gs_update *updates;

static time_t gs_time( INT64 value )
{
    if (value <= 0) return 0;
    if (sizeof(time_t) < sizeof(INT64) && value > 0x7fffffff) return 0x7fffffff;
    return value;
}

static BOOL valid_container_name( const char *name )
{
    SIZE_T length;
    const char *c;
    if (!name || !(length = strlen( name )) || length > GS_MAX_CONTAINER_NAME) return FALSE;
    for (c = name; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '/'))
            return FALSE;
    return TRUE;
}

static BOOL valid_blob_name( const char *name )
{
    SIZE_T length;
    const char *c;
    if (!name || !(length = strlen( name )) || length > GS_MAX_BLOB_NAME) return FALSE;
    for (c = name; *c; ++c) if ((unsigned char)*c < 0x20 || *c == 0x7f) return FALSE;
    return TRUE;
}

static BOOL valid_scid( const char *scid )
{
    SIZE_T length;
    const char *c;
    if (!scid || !(length = strlen( scid )) || length > 64) return FALSE;
    for (c = scid; *c; ++c)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '-'))
            return FALSE;
    return TRUE;
}

static void provider_release( struct gs_provider *p )
{
    if (!InterlockedDecrement( &p->refs )) free( p );
}

static void container_release( struct gs_container *c )
{
    UINT32 i;
    if (InterlockedDecrement( &c->refs )) return;
    for (i = 0; i < c->pool_count; ++i) free( c->pool[i] );
    free( c->pool );
    provider_release( c->provider );
    free( c );
}

static struct gs_provider *provider_acquire( XGameSaveProviderHandle handle )
{
    struct gs_provider *p;
    AcquireSRWLockShared( &gs_lock );
    for (p = providers; p && (XGameSaveProviderHandle)p != handle; p = p->next) {}
    if (p) InterlockedIncrement( &p->refs );
    ReleaseSRWLockShared( &gs_lock );
    return p;
}

static struct gs_container *container_acquire( XGameSaveContainerHandle handle )
{
    struct gs_container *c;
    AcquireSRWLockShared( &gs_lock );
    for (c = containers; c && (XGameSaveContainerHandle)c != handle; c = c->next) {}
    if (c) InterlockedIncrement( &c->refs );
    ReleaseSRWLockShared( &gs_lock );
    return c;
}

static HRESULT win32_hr( DWORD error )
{
    if (error == ERROR_DISK_FULL || error == ERROR_HANDLE_DISK_FULL) return E_GS_OUT_OF_LOCAL_STORAGE;
    return HRESULT_FROM_WIN32( error ? error : ERROR_GEN_FAILURE );
}

static HRESULT ensure_directory( WCHAR *path )
{
    WCHAR *p;
    for (p = path + 3; *p; ++p)
    {
        if (*p != '\\') continue;
        *p = 0;
        if (!CreateDirectoryW( path, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) { *p = '\\'; return win32_hr( GetLastError() ); }
        *p = '\\';
    }
    if (!CreateDirectoryW( path, NULL ) && GetLastError() != ERROR_ALREADY_EXISTS) return win32_hr( GetLastError() );
    return S_OK;
}

static void free_image( struct gs_image *img )
{
    free( img->blobs );
    free( img->raw );
    memset( img, 0, sizeof(*img) );
}

static BOOL take( const BYTE **p, const BYTE *end, SIZE_T size, const BYTE **out )
{
    if ((SIZE_T)(end - *p) < size) return FALSE;
    *out = *p;
    *p += size;
    return TRUE;
}

static BOOL take_u32( const BYTE **p, const BYTE *end, UINT32 *value )
{
    const BYTE *v;
    if (!take( p, end, 4, &v )) return FALSE;
    memcpy( value, v, 4 );
    return TRUE;
}

static BOOL take_string( const BYTE **p, const BYTE *end, const char **value )
{
    const BYTE *v;
    UINT32 length;
    if (!take_u32( p, end, &length ) || !length || !take( p, end, length, &v ) || v[length - 1] ||
        memchr( v, 0, length - 1 )) return FALSE;
    *value = (const char *)v;
    return TRUE;
}

/* How much of a container file to read: names only (slot lookups), metadata
 * (names, sizes, no data) or everything. */
enum gs_read_mode { READ_NAME, READ_META, READ_ALL };
#define GS_NAME_BYTES 4096
#define GS_META_BYTES (16 + GS_NAME_BYTES + 8 + 4 + GS_MAX_BLOBS * (4 + GS_MAX_BLOB_NAME + 1 + 4))

/* Reads a container file. S_FALSE: the file does not exist. */
static HRESULT read_image( const WCHAR *path, struct gs_image *img, enum gs_read_mode mode )
{
    LARGE_INTEGER size;
    const BYTE *p, *end, *v;
    UINT32 magic, version, i;
    DWORD read, want;
    HANDLE file;
    HRESULT hr = HRESULT_FROM_WIN32( ERROR_FILE_CORRUPT );
    memset( img, 0, sizeof(*img) );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? S_FALSE : win32_hr( GetLastError() );
    if (!GetFileSizeEx( file, &size ) || size.QuadPart > GS_QUOTA + 16 * 1024 * 1024)
    {
        CloseHandle( file );
        return HRESULT_FROM_WIN32( ERROR_FILE_CORRUPT );
    }
    want = (DWORD)size.QuadPart;
    if (mode == READ_NAME && want > GS_NAME_BYTES) want = GS_NAME_BYTES;
    if (mode == READ_META && want > GS_META_BYTES) want = GS_META_BYTES;
    if (!(img->raw = malloc( want ? want : 1 ))) { CloseHandle( file ); return E_OUTOFMEMORY; }
    if (!ReadFile( file, img->raw, want, &read, NULL ) || read != want)
    {
        hr = win32_hr( GetLastError() );
        CloseHandle( file );
        free_image( img );
        return hr;
    }
    CloseHandle( file );
    p = img->raw;
    end = img->raw + want;
    if (!take_u32( &p, end, &magic ) || magic != GS_MAGIC || !take_u32( &p, end, &version ) || version != 1 ||
        !take_string( &p, end, &img->name ) || !take_string( &p, end, &img->display ))
        goto failed;
    if (mode == READ_NAME) return S_OK;
    if (!take( &p, end, 8, &v )) goto failed;
    memcpy( &img->modified, v, 8 );
    if (!take_u32( &p, end, &img->count ) || img->count > GS_MAX_BLOBS) goto failed;
    if (img->count && !(img->blobs = calloc( img->count, sizeof(*img->blobs) ))) { free_image( img ); return E_OUTOFMEMORY; }
    for (i = 0; i < img->count; ++i)
        if (!take_string( &p, end, &img->blobs[i].name ) || !take_u32( &p, end, &img->blobs[i].size )) goto failed;
    if (mode == READ_META)
    {
        UINT64 data = 0;
        for (i = 0; i < img->count; ++i) data += img->blobs[i].size;
        if ((UINT64)size.QuadPart != (UINT64)(p - img->raw) + data) goto failed;
        return S_OK; /* blob data pointers stay NULL */
    }
    for (i = 0; i < img->count; ++i)
        if (!take( &p, end, img->blobs[i].size, &img->blobs[i].data )) goto failed;
    if (p != end) goto failed;
    return S_OK;
failed:
    free_image( img );
    return hr;
}

static void put_raw( BYTE **p, const void *data, SIZE_T size )
{
    memcpy( *p, data, size );
    *p += size;
}

static HRESULT write_image( const WCHAR *path, const struct gs_image *img )
{
    WCHAR tmp[MAX_PATH + 8];
    UINT32 value, i;
    SIZE_T size = 4 + 4 + 4 + strlen( img->name ) + 1 + 4 + strlen( img->display ) + 1 + 8 + 4;
    BYTE *buffer, *p;
    DWORD written;
    HANDLE file;
    HRESULT hr = S_OK;
    for (i = 0; i < img->count; ++i) size += 4 + strlen( img->blobs[i].name ) + 1 + 4 + img->blobs[i].size;
    if (!(buffer = malloc( size ))) return E_OUTOFMEMORY;
    p = buffer;
    value = GS_MAGIC; put_raw( &p, &value, 4 );
    value = 1; put_raw( &p, &value, 4 );
    value = strlen( img->name ) + 1; put_raw( &p, &value, 4 ); put_raw( &p, img->name, value );
    value = strlen( img->display ) + 1; put_raw( &p, &value, 4 ); put_raw( &p, img->display, value );
    put_raw( &p, &img->modified, 8 );
    put_raw( &p, &img->count, 4 );
    for (i = 0; i < img->count; ++i)
    {
        value = strlen( img->blobs[i].name ) + 1; put_raw( &p, &value, 4 ); put_raw( &p, img->blobs[i].name, value );
        put_raw( &p, &img->blobs[i].size, 4 );
    }
    for (i = 0; i < img->count; ++i) if (img->blobs[i].size) put_raw( &p, img->blobs[i].data, img->blobs[i].size );
    lstrcpyW( tmp, path );
    lstrcatW( tmp, L".tmp" );
    file = CreateFileW( tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
    if (file == INVALID_HANDLE_VALUE) hr = win32_hr( GetLastError() );
    else
    {
        if (!WriteFile( file, buffer, size, &written, NULL ) || written != size || !FlushFileBuffers( file ))
            hr = win32_hr( GetLastError() );
        CloseHandle( file );
        if (SUCCEEDED(hr) && !MoveFileExW( tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH ))
            hr = win32_hr( GetLastError() );
        if (FAILED(hr)) DeleteFileW( tmp );
    }
    free( buffer );
    return hr;
}

static UINT64 name_hash( const char *name )
{
    UINT64 hash = 0xcbf29ce484222325ull;
    for (; *name; ++name) hash = (hash ^ (unsigned char)*name) * 0x100000001b3ull;
    return hash;
}

/* Finds the file of a container (S_OK) or the first free slot for it (S_FALSE). */
static HRESULT container_path( const struct gs_provider *p, const char *name, WCHAR *path )
{
    WCHAR candidate[MAX_PATH];
    struct gs_image img;
    UINT64 hash = name_hash( name );
    BOOL have_free = FALSE;
    unsigned int slot;
    HRESULT hr;
    for (slot = 0; slot < GS_SLOTS; ++slot)
    {
        swprintf( candidate, ARRAY_SIZE(candidate), L"%s\\%08x%08x-%u.xgs", p->root,
                  (unsigned int)(hash >> 32), (unsigned int)hash, slot );
        if (FAILED(hr = read_image( candidate, &img, READ_NAME ))) return hr;
        if (hr == S_FALSE)
        {
            if (!have_free) { lstrcpyW( path, candidate ); have_free = TRUE; }
            continue;
        }
        hr = strcmp( img.name, name ) ? S_FALSE : S_OK;
        free_image( &img );
        if (hr == S_OK) { lstrcpyW( path, candidate ); return S_OK; }
    }
    return have_free ? S_FALSE : E_GS_OUT_OF_LOCAL_STORAGE;
}

typedef BOOL (*image_callback)( const struct gs_image *img, void *context );

/* Calls back with the metadata of every readable container; stops when the
 * callback returns FALSE. */
static HRESULT for_each_image( const struct gs_provider *p, image_callback callback, void *context )
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW data;
    struct gs_image img;
    HANDLE find;
    HRESULT hr = S_OK;
    BOOL more = TRUE;
    swprintf( pattern, ARRAY_SIZE(pattern), L"%s\\*.xgs", p->root );
    if ((find = FindFirstFileW( pattern, &data )) == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND ? S_OK : win32_hr( GetLastError() );
    do
    {
        swprintf( path, ARRAY_SIZE(path), L"%s\\%s", p->root, data.cFileName );
        if ((hr = read_image( path, &img, READ_META )) == S_OK)
        {
            more = callback( &img, context );
            free_image( &img );
        }
        else if (hr == S_FALSE) hr = S_OK; /* removed meanwhile */
        else WARN( "skipping unreadable container file %s hr %#lx.\n", debugstr_w( data.cFileName ), hr );
        hr = S_OK;
    } while (more && FindNextFileW( find, &data ));
    FindClose( find );
    return hr;
}

static UINT64 image_size( const struct gs_image *img )
{
    UINT64 size = 0;
    UINT32 i;
    for (i = 0; i < img->count; ++i) size += img->blobs[i].size;
    return size;
}

struct usage_context { const char *skip; UINT64 used; };
static BOOL add_usage( const struct gs_image *img, void *context )
{
    struct usage_context *u = context;
    if (!u->skip || strcmp( img->name, u->skip )) u->used += image_size( img );
    return TRUE;
}

static HRESULT provider_usage( const struct gs_provider *p, const char *skip, UINT64 *used )
{
    struct usage_context u = { skip, 0 };
    HRESULT hr = for_each_image( p, add_usage, &u );
    *used = u.used;
    return hr;
}

static HRESULT initialize_provider( UINT64 xuid, const char *scid, struct gs_provider **out )
{
    struct gs_provider *p;
    WCHAR base[MAX_PATH];
    DWORD length;
    HRESULT hr;
    *out = NULL;
    length = GetEnvironmentVariableW( L"LOCALAPPDATA", base, ARRAY_SIZE(base) );
    if (!length || length >= ARRAY_SIZE(base) - 128) return E_GS_NO_ACCESS;
    if (!(p = calloc( 1, sizeof(*p) ))) return E_OUTOFMEMORY;
    swprintf( p->root, ARRAY_SIZE(p->root), L"%s\\Xodus\\GameSave\\%I64u\\%S", base, xuid, scid );
    if (FAILED(hr = ensure_directory( p->root ))) { free( p ); return hr; }
    InitializeSRWLock( &p->io );
    p->refs = 1;
    AcquireSRWLockExclusive( &gs_lock );
    p->next = providers;
    providers = p;
    ReleaseSRWLockExclusive( &gs_lock );
    *out = p;
    return S_OK;
}

static HRESULT remaining_quota( struct gs_provider *p, INT64 *remaining )
{
    UINT64 used;
    HRESULT hr;
    AcquireSRWLockShared( &p->io );
    hr = provider_usage( p, NULL, &used );
    ReleaseSRWLockShared( &p->io );
    *remaining = SUCCEEDED(hr) ? (used >= GS_QUOTA ? 0 : GS_QUOTA - used) : 0;
    return hr;
}

static HRESULT delete_container( struct gs_provider *p, const char *name )
{
    WCHAR path[MAX_PATH];
    HRESULT hr;
    if (!valid_container_name( name )) return E_GS_INVALID_CONTAINER_NAME;
    AcquireSRWLockExclusive( &p->io );
    if ((hr = container_path( p, name, path )) == S_OK && !DeleteFileW( path )) hr = win32_hr( GetLastError() );
    ReleaseSRWLockExclusive( &p->io );
    return FAILED(hr) ? hr : S_OK; /* deleting an absent container is not an error */
}

/* Read plan: the requested blobs (all when names is NULL) of a container. */
struct gs_read
{
    struct gs_image img;
    UINT32 count;
    UINT32 *index;
    SIZE_T size;
};

static void free_read( struct gs_read *r )
{
    free_image( &r->img );
    free( r->index );
    memset( r, 0, sizeof(*r) );
}

static HRESULT plan_read( struct gs_container *c, const char **names, UINT32 count, enum gs_read_mode mode, struct gs_read *r )
{
    WCHAR path[MAX_PATH];
    UINT32 i, j;
    HRESULT hr;
    memset( r, 0, sizeof(*r) );
    if (names) for (i = 0; i < count; ++i) if (!valid_blob_name( names[i] )) return E_INVALIDARG;
    AcquireSRWLockShared( &c->provider->io );
    if ((hr = container_path( c->provider, c->name, path )) == S_OK) hr = read_image( path, &r->img, mode );
    ReleaseSRWLockShared( &c->provider->io );
    if (FAILED(hr)) return hr;
    if (hr == S_FALSE) return names && count ? E_GS_BLOB_NOT_FOUND : S_OK;
    r->count = names ? count : r->img.count;
    if (r->count && !(r->index = calloc( r->count, sizeof(*r->index) ))) { free_read( r ); return E_OUTOFMEMORY; }
    for (i = 0; i < r->count; ++i)
    {
        if (names)
        {
            for (j = 0; j < r->img.count && strcmp( r->img.blobs[j].name, names[i] ); ++j) {}
            if (j == r->img.count) { free_read( r ); return E_GS_BLOB_NOT_FOUND; }
        }
        else j = i;
        r->index[i] = j;
        r->size += sizeof(XGameSaveBlob) + r->img.blobs[j].size;
    }
    return S_OK;
}

static const char *pooled_name( struct gs_container *c, const char *name )
{
    char **grown, *copy;
    UINT32 i;
    AcquireSRWLockExclusive( &c->pool_lock );
    for (i = 0; i < c->pool_count && strcmp( c->pool[i], name ); ++i) {}
    if (i < c->pool_count) copy = c->pool[i];
    else if ((copy = strdup( name )) && (grown = realloc( c->pool, (c->pool_count + 1) * sizeof(*c->pool) )))
    {
        c->pool = grown;
        c->pool[c->pool_count++] = copy;
    }
    else { free( copy ); copy = NULL; }
    ReleaseSRWLockExclusive( &c->pool_lock );
    return copy;
}

/* Layout (as GDK samples size it): the blob array, then each blob's data.
 * Blob names stay owned by the container handle. */
static HRESULT fill_read( struct gs_container *c, const struct gs_read *r, SIZE_T size, XGameSaveBlob *blobs, UINT32 *count )
{
    BYTE *data = (BYTE *)(blobs + r->count);
    const struct gs_blob *b;
    UINT32 i;
    if (size < r->size) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    if (r->count && !blobs) return E_INVALIDARG;
    for (i = 0; i < r->count; ++i)
    {
        b = &r->img.blobs[r->index[i]];
        if (!(blobs[i].info.name = pooled_name( c, b->name ))) return E_OUTOFMEMORY;
        blobs[i].info.size = b->size;
        blobs[i].data = data;
        if (b->size) memcpy( data, b->data, b->size );
        data += b->size;
    }
    *count = r->count;
    return S_OK;
}

static HRESULT submit_update( struct gs_update *u )
{
    struct gs_container *c = u->container;
    struct gs_provider *p = c->provider;
    struct gs_image old, img;
    WCHAR path[MAX_PATH];
    UINT64 others, total = 0;
    UINT32 i, j;
    HRESULT hr;
    if (u->submitted) return E_INVALIDARG;
    memset( &img, 0, sizeof(img) );
    AcquireSRWLockExclusive( &p->io );
    if (FAILED(hr = container_path( p, c->name, path ))) goto done;
    if (hr == S_OK && FAILED(hr = read_image( path, &old, READ_ALL ))) goto done;
    if (hr == S_FALSE) memset( &old, 0, sizeof(old) );
    img.name = c->name;
    img.display = u->display ? u->display : old.display ? old.display : c->name;
    img.modified = time( NULL );
    if (!(img.blobs = calloc( old.count + u->count + 1, sizeof(*img.blobs) ))) { hr = E_OUTOFMEMORY; goto done_old; }
    for (i = 0; i < old.count; ++i) img.blobs[img.count++] = old.blobs[i];
    for (i = 0; i < u->count; ++i)
    {
        for (j = 0; j < img.count && strcmp( img.blobs[j].name, u->ops[i].name ); ++j) {}
        if (u->ops[i].remove)
        {
            if (j == img.count) { hr = E_GS_BLOB_NOT_FOUND; goto done_old; }
            memmove( &img.blobs[j], &img.blobs[j + 1], (img.count - j - 1) * sizeof(*img.blobs) );
            --img.count;
            continue;
        }
        if (j == img.count && img.count == GS_MAX_BLOBS) { hr = E_GS_UPDATE_TOO_BIG; goto done_old; }
        img.blobs[j].name = u->ops[i].name;
        img.blobs[j].size = u->ops[i].size;
        img.blobs[j].data = u->ops[i].data;
        if (j == img.count) ++img.count;
    }
    for (i = 0; i < img.count; ++i) total += img.blobs[i].size;
    if (FAILED(hr = provider_usage( p, c->name, &others ))) goto done_old;
    if (others + total > (UINT64)GS_QUOTA) { hr = E_GS_QUOTA_EXCEEDED; goto done_old; }
    if (SUCCEEDED(hr = write_image( path, &img ))) u->submitted = TRUE;
done_old:
    free( img.blobs );
    free_image( &old );
done:
    ReleaseSRWLockExclusive( &p->io );
    TRACE( "container %s update ops %u hr %#lx.\n", debugstr_a( c->name ), u->count, hr );
    return hr;
}

static void free_update( struct gs_update *u )
{
    UINT32 i;
    for (i = 0; i < u->count; ++i) { free( u->ops[i].name ); free( u->ops[i].data ); }
    free( u->ops );
    free( u->display );
    container_release( u->container );
    free( u );
}

static struct gs_update *update_find( XGameSaveUpdateHandle handle )
{
    struct gs_update *u;
    for (u = updates; u && (XGameSaveUpdateHandle)u != handle; u = u->next) {}
    return u;
}

/* Asynchronous variants run the synchronous implementation as work. */
enum gs_op_kind { GS_INIT, GS_QUOTA_OP, GS_DELETE, GS_READ, GS_SUBMIT };
struct gs_async
{
    enum gs_op_kind kind;
    UINT64 xuid;
    char *text; /* SCID or container name */
    char **names;
    UINT32 name_count;
    BOOL all;
    struct gs_provider *provider; /* reference held for GS_QUOTA_OP / GS_DELETE */
    struct gs_provider *result;   /* GS_INIT result until retrieved */
    struct gs_container *container;
    XGameSaveUpdateHandle update;
    INT64 quota;
    struct gs_read read;
};
static const char gs_init_identity, gs_quota_identity, gs_delete_identity, gs_read_identity, gs_submit_identity;

static void close_provider( struct gs_provider *p );

static void gs_async_free( void *context )
{
    struct gs_async *a = context;
    UINT32 i;
    if (a->result) close_provider( a->result ); /* never retrieved */
    if (a->provider) provider_release( a->provider );
    if (a->container) container_release( a->container );
    for (i = 0; i < a->name_count; ++i) free( a->names[i] );
    free( a->names );
    free( a->text );
    free_read( &a->read );
    free( a );
}

static HRESULT run_submit( XGameSaveUpdateHandle handle )
{
    struct gs_update *u;
    HRESULT hr;
    AcquireSRWLockShared( &gs_lock );
    u = update_find( handle );
    if (u) InterlockedIncrement( &u->container->refs );
    ReleaseSRWLockShared( &gs_lock );
    if (!u) return E_INVALIDARG;
    /* The caller must not close an update while it is being submitted. */
    hr = submit_update( u );
    container_release( u->container );
    return hr;
}

static HRESULT WINAPI gs_provider_cb( XAsyncOp op, const XAsyncProviderData *data )
{
    struct gs_async *a = data->context;
    SIZE_T size = 0;
    UINT32 count;
    HRESULT hr;
    switch (op)
    {
    case XAsyncOp_Begin:
        return async_schedule( data->async, 0 );
    case XAsyncOp_DoWork:
        switch (a->kind)
        {
        case GS_INIT: hr = initialize_provider( a->xuid, a->text, &a->result ); size = sizeof(XGameSaveProviderHandle); break;
        case GS_QUOTA_OP: hr = remaining_quota( a->provider, &a->quota ); size = sizeof(INT64); break;
        case GS_DELETE: hr = delete_container( a->provider, a->text ); break;
        case GS_READ: hr = plan_read( a->container, a->all ? NULL : (const char **)a->names, a->name_count, READ_ALL, &a->read ); size = a->read.size; break;
        default: hr = run_submit( a->update ); break;
        }
        async_complete( data->async, hr, SUCCEEDED(hr) ? size : 0 );
        return E_PENDING;
    case XAsyncOp_GetResult:
        switch (a->kind)
        {
        case GS_INIT: *(XGameSaveProviderHandle *)data->buffer = (XGameSaveProviderHandle)a->result; a->result = NULL; return S_OK;
        case GS_QUOTA_OP: *(INT64 *)data->buffer = a->quota; return S_OK;
        case GS_READ: return fill_read( a->container, &a->read, data->bufferSize, data->buffer, &count );
        default: return S_OK;
        }
    case XAsyncOp_Cancel:
        async_complete( data->async, E_ABORT, 0 );
        return S_OK;
    case XAsyncOp_Cleanup:
        gs_async_free( a );
        return S_OK;
    }
    return E_NOTIMPL;
}

static HRESULT gs_begin( struct gs_async *a, const void *identity, XAsyncBlock *async )
{
    return async_begin_owned( async, a, identity, gs_provider_cb, gs_async_free );
}

static void close_provider( struct gs_provider *p )
{
    struct gs_provider **link;
    AcquireSRWLockExclusive( &gs_lock );
    for (link = &providers; *link && *link != p; link = &(*link)->next) {}
    if (*link) *link = p->next;
    else p = NULL;
    ReleaseSRWLockExclusive( &gs_lock );
    if (p) provider_release( p );
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProvider( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XGameSaveProviderHandle *provider )
{
    struct gs_provider *p;
    UINT64 xuid;
    HRESULT hr;
    if (!provider) return E_POINTER;
    *provider = NULL;
    if (!requestingUser || !valid_scid( configurationId )) return E_INVALIDARG;
    if (FAILED(hr = user_account( requestingUser, &xuid ))) return hr;
    if (SUCCEEDED(hr = initialize_provider( xuid, configurationId, &p ))) *provider = (XGameSaveProviderHandle)p;
    TRACE( "local provider for %s syncOnDemand %d hr %#lx.\n", debugstr_a( configurationId ), syncOnDemand, hr );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, BOOLEAN syncOnDemand, XAsyncBlock *async )
{
    struct gs_async *a;
    UINT64 xuid;
    HRESULT hr;
    if (!async || !requestingUser || !valid_scid( configurationId )) return E_INVALIDARG;
    if (FAILED(hr = user_account( requestingUser, &xuid ))) return hr;
    if (!(a = calloc( 1, sizeof(*a) )) || !(a->text = strdup( configurationId ))) { free( a ); return E_OUTOFMEMORY; }
    a->kind = GS_INIT;
    a->xuid = xuid;
    return gs_begin( a, &gs_init_identity, async );
}

static HRESULT WINAPI x_game_save_XGameSaveInitializeProviderResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, XGameSaveProviderHandle *provider )
{
    if (!provider) return E_POINTER;
    *provider = NULL;
    return async_result( async, &gs_init_identity, sizeof(*provider), provider, NULL );
}

static void WINAPI x_game_save_XGameSaveCloseProvider( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider )
{
    struct gs_provider *p;
    if (!(p = provider_acquire( provider ))) return;
    close_provider( p );
    provider_release( p );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuota( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, INT64 *remainingQuota )
{
    struct gs_provider *p;
    HRESULT hr;
    if (!remainingQuota) return E_POINTER;
    *remainingQuota = 0;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    hr = remaining_quota( p, remainingQuota );
    provider_release( p );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, XAsyncBlock *async )
{
    struct gs_async *a;
    struct gs_provider *p;
    if (!async) return E_INVALIDARG;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    if (!(a = calloc( 1, sizeof(*a) ))) { provider_release( p ); return E_OUTOFMEMORY; }
    a->kind = GS_QUOTA_OP;
    a->provider = p;
    return gs_begin( a, &gs_quota_identity, async );
}

static HRESULT WINAPI x_game_save_XGameSaveGetRemainingQuotaResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, INT64 *remainingQuota )
{
    if (!remainingQuota) return E_POINTER;
    *remainingQuota = 0;
    return async_result( async, &gs_quota_identity, sizeof(*remainingQuota), remainingQuota, NULL );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName )
{
    struct gs_provider *p;
    HRESULT hr;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    hr = delete_container( p, containerName );
    provider_release( p );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerAsync( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XAsyncBlock *async )
{
    struct gs_async *a;
    struct gs_provider *p;
    if (!async) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    if (!(a = calloc( 1, sizeof(*a) )) || !(a->text = strdup( containerName )))
    {
        free( a );
        provider_release( p );
        return E_OUTOFMEMORY;
    }
    a->kind = GS_DELETE;
    a->provider = p;
    return gs_begin( a, &gs_delete_identity, async );
}

static HRESULT WINAPI x_game_save_XGameSaveDeleteContainerResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    /* Operations without a result payload are released at completion. */
    return async_status( async, FALSE );
}

static void fill_info( const struct gs_image *img, XGameSaveContainerInfo *info )
{
    info->name = img->name;
    info->displayName = img->display;
    info->blobCount = img->count;
    info->totalSize = image_size( img );
    info->lastModifiedTime = gs_time( img->modified );
    info->needsSync = FALSE;
}

struct info_context
{
    const char *prefix;
    void *context;
    XGameSaveContainerInfoCallback *callback;
};
static BOOL report_info( const struct gs_image *img, void *context )
{
    struct info_context *i = context;
    XGameSaveContainerInfo info;
    if (i->prefix && strncmp( img->name, i->prefix, strlen( i->prefix ) )) return TRUE;
    fill_info( img, &info );
    return i->callback( &info, i->context );
}

static HRESULT enumerate_containers( XGameSaveProviderHandle provider, const char *prefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    struct info_context i = { prefix, context, callback };
    struct gs_provider *p;
    HRESULT hr;
    if (!callback) return E_INVALIDARG;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    AcquireSRWLockShared( &p->io );
    hr = for_each_image( p, report_info, &i );
    ReleaseSRWLockShared( &p->io );
    provider_release( p );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveGetContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, void *context, XGameSaveContainerInfoCallback *callback )
{
    XGameSaveContainerInfo info;
    struct gs_provider *p;
    struct gs_image img;
    WCHAR path[MAX_PATH];
    HRESULT hr;
    if (!callback) return E_INVALIDARG;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    AcquireSRWLockShared( &p->io );
    if ((hr = container_path( p, containerName, path )) == S_OK) hr = read_image( path, &img, READ_META );
    ReleaseSRWLockShared( &p->io );
    provider_release( p );
    if (hr != S_OK) return FAILED(hr) ? hr : S_OK; /* no callback when absent */
    fill_info( &img, &info );
    callback( &info, context );
    free_image( &img );
    return S_OK;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfo( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, void *context, XGameSaveContainerInfoCallback *callback )
{
    return enumerate_containers( provider, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateContainerInfoByName( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerNamePrefix, void *context, XGameSaveContainerInfoCallback *callback )
{
    return enumerate_containers( provider, containerNamePrefix && *containerNamePrefix ? containerNamePrefix : NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveCreateContainer( IXGameSaveImpl3 *iface, XGameSaveProviderHandle provider, const char *containerName, XGameSaveContainerHandle *containerContext )
{
    struct gs_container *c;
    struct gs_provider *p;
    if (!containerContext) return E_POINTER;
    *containerContext = NULL;
    if (!valid_container_name( containerName )) return E_GS_INVALID_CONTAINER_NAME;
    if (!(p = provider_acquire( provider ))) return E_INVALIDARG;
    if (!(c = calloc( 1, sizeof(*c) ))) { provider_release( p ); return E_OUTOFMEMORY; }
    c->refs = 1;
    c->provider = p; /* the acquired reference is owned by the container */
    strcpy( c->name, containerName );
    InitializeSRWLock( &c->pool_lock );
    AcquireSRWLockExclusive( &gs_lock );
    c->next = containers;
    containers = c;
    ReleaseSRWLockExclusive( &gs_lock );
    *containerContext = (XGameSaveContainerHandle)c;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseContainer( IXGameSaveImpl3 *iface, XGameSaveContainerHandle context )
{
    struct gs_container **link, *c;
    AcquireSRWLockExclusive( &gs_lock );
    for (link = &containers; *link && (XGameSaveContainerHandle)*link != context; link = &(*link)->next) {}
    if ((c = *link)) *link = c->next;
    ReleaseSRWLockExclusive( &gs_lock );
    if (c) container_release( c );
}

static HRESULT enumerate_blobs( XGameSaveContainerHandle container, const char *prefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    XGameSaveBlobInfo info;
    struct gs_container *c;
    struct gs_read r;
    UINT32 i;
    HRESULT hr;
    if (!callback) return E_INVALIDARG;
    if (!(c = container_acquire( container ))) return E_INVALIDARG;
    if (SUCCEEDED(hr = plan_read( c, NULL, 0, READ_META, &r )))
    {
        for (i = 0; i < r.img.count; ++i)
        {
            if (prefix && strncmp( r.img.blobs[i].name, prefix, strlen( prefix ) )) continue;
            info.name = r.img.blobs[i].name;
            info.size = r.img.blobs[i].size;
            if (!callback( &info, context )) break;
        }
        free_read( &r );
    }
    container_release( c );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfo( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, void *context, XGameSaveBlobInfoCallback *callback )
{
    return enumerate_blobs( container, NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveEnumerateBlobInfoByName( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *blobNamePrefix, void *context, XGameSaveBlobInfoCallback *callback )
{
    return enumerate_blobs( container, blobNamePrefix && *blobNamePrefix ? blobNamePrefix : NULL, context, callback );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobData( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 *countOfBlobs, SIZE_T blobsSize, XGameSaveBlob *blobData )
{
    struct gs_container *c;
    struct gs_read r;
    UINT32 requested;
    HRESULT hr;
    if (!countOfBlobs) return E_POINTER;
    requested = *countOfBlobs;
    if (blobNames && !requested) return E_INVALIDARG;
    if (!(c = container_acquire( container ))) return E_INVALIDARG;
    if (SUCCEEDED(hr = plan_read( c, blobNames, requested, READ_ALL, &r )))
    {
        if (!blobNames && r.count > requested && requested) hr = E_GS_PROVIDED_BUFFER_TOO_SMALL;
        else hr = fill_read( c, &r, blobsSize, blobData, countOfBlobs );
        free_read( &r );
    }
    container_release( c );
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataAsync( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char **blobNames, UINT32 countOfBlobs, XAsyncBlock *async )
{
    struct gs_container *c;
    struct gs_async *a;
    UINT32 i;
    if (!async || (blobNames && !countOfBlobs)) return E_INVALIDARG;
    if (blobNames) for (i = 0; i < countOfBlobs; ++i) if (!valid_blob_name( blobNames[i] )) return E_INVALIDARG;
    if (!(c = container_acquire( container ))) return E_INVALIDARG;
    if (!(a = calloc( 1, sizeof(*a) ))) { container_release( c ); return E_OUTOFMEMORY; }
    a->kind = GS_READ;
    a->container = c;
    a->all = !blobNames;
    if (blobNames)
    {
        if (!(a->names = calloc( countOfBlobs, sizeof(*a->names) ))) { gs_async_free( a ); return E_OUTOFMEMORY; }
        for (i = 0; i < countOfBlobs; ++i, ++a->name_count)
            if (!(a->names[i] = strdup( blobNames[i] ))) { gs_async_free( a ); return E_OUTOFMEMORY; }
    }
    return gs_begin( a, &gs_read_identity, async );
}

static HRESULT WINAPI x_game_save_XGameSaveReadBlobDataResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T blobsSize, XGameSaveBlob *blobData, UINT32 *countOfBlobs )
{
    SIZE_T needed = 0;
    HRESULT hr;
    if (!countOfBlobs) return E_POINTER;
    *countOfBlobs = 0;
    if (SUCCEEDED(async_size( async, &needed )) && blobsSize < needed) return E_GS_PROVIDED_BUFFER_TOO_SMALL;
    /* The blob array leads the buffer and the first blob's data follows it. */
    if (SUCCEEDED(hr = async_result( async, &gs_read_identity, blobsSize, blobData, NULL )) && needed)
        *countOfBlobs = (UINT32)(((BYTE *)blobData[0].data - (BYTE *)blobData) / sizeof(*blobData));
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveCreateUpdate( IXGameSaveImpl3 *iface, XGameSaveContainerHandle container, const char *containerDisplayName, XGameSaveUpdateHandle *updateContext )
{
    struct gs_container *c;
    struct gs_update *u;
    if (!updateContext) return E_POINTER;
    *updateContext = NULL;
    if (containerDisplayName && strlen( containerDisplayName ) > 1024) return E_INVALIDARG;
    if (!(c = container_acquire( container ))) return E_INVALIDARG;
    if (!(u = calloc( 1, sizeof(*u) )) || (containerDisplayName && !(u->display = strdup( containerDisplayName ))))
    {
        free( u );
        container_release( c );
        return E_OUTOFMEMORY;
    }
    u->container = c; /* owns the acquired reference */
    AcquireSRWLockExclusive( &gs_lock );
    u->next = updates;
    updates = u;
    ReleaseSRWLockExclusive( &gs_lock );
    *updateContext = (XGameSaveUpdateHandle)u;
    return S_OK;
}

static void WINAPI x_game_save_XGameSaveCloseUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle context )
{
    struct gs_update **link, *u;
    AcquireSRWLockExclusive( &gs_lock );
    for (link = &updates; *link && (XGameSaveUpdateHandle)*link != context; link = &(*link)->next) {}
    if ((u = *link)) *link = u->next;
    ReleaseSRWLockExclusive( &gs_lock );
    if (u) free_update( u );
}

static HRESULT add_op( XGameSaveUpdateHandle handle, const char *name, const UINT8 *data, SIZE_T size, BOOL remove )
{
    struct gs_update *u;
    struct gs_op *grown;
    char *copy = NULL;
    BYTE *bytes = NULL;
    HRESULT hr = S_OK;
    if (!valid_blob_name( name ) || (!remove && size && !data)) return E_INVALIDARG;
    if (!remove && size > GS_MAX_UPDATE_SIZE) return E_GS_UPDATE_TOO_BIG;
    if (!(copy = strdup( name )) || (!remove && size && !(bytes = malloc( size ))))
    {
        free( copy );
        return E_OUTOFMEMORY;
    }
    if (bytes) memcpy( bytes, data, size );
    AcquireSRWLockExclusive( &gs_lock );
    if (!(u = update_find( handle )) || u->submitted) hr = E_INVALIDARG;
    else if (!remove && u->written + size > GS_MAX_UPDATE_SIZE) hr = E_GS_UPDATE_TOO_BIG;
    else if (u->count == u->capacity)
    {
        if ((grown = realloc( u->ops, (u->capacity ? u->capacity * 2 : 8) * sizeof(*u->ops) )))
        {
            u->ops = grown;
            u->capacity = u->capacity ? u->capacity * 2 : 8;
        }
        else hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr))
    {
        u->ops[u->count].name = copy;
        u->ops[u->count].data = bytes;
        u->ops[u->count].size = remove ? 0 : size;
        u->ops[u->count].remove = remove;
        ++u->count;
        if (!remove) u->written += size;
    }
    ReleaseSRWLockExclusive( &gs_lock );
    if (FAILED(hr)) { free( copy ); free( bytes ); }
    return hr;
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobWrite( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName, UINT8 *data, SIZE_T byteCount )
{
    return add_op( updateContext, blobName, data, byteCount, FALSE );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitBlobDelete( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, const char *blobName )
{
    return add_op( updateContext, blobName, NULL, 0, TRUE );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdate( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext )
{
    return run_submit( updateContext );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateAsync( IXGameSaveImpl3 *iface, XGameSaveUpdateHandle updateContext, XAsyncBlock *async )
{
    struct gs_async *a;
    BOOL known;
    if (!async) return E_INVALIDARG;
    AcquireSRWLockShared( &gs_lock );
    known = update_find( updateContext ) != NULL;
    ReleaseSRWLockShared( &gs_lock );
    if (!known) return E_INVALIDARG;
    if (!(a = calloc( 1, sizeof(*a) ))) return E_OUTOFMEMORY;
    a->kind = GS_SUBMIT;
    a->update = updateContext;
    return gs_begin( a, &gs_submit_identity, async );
}

static HRESULT WINAPI x_game_save_XGameSaveSubmitUpdateResult( IXGameSaveImpl3 *iface, XAsyncBlock *async )
{
    /* Operations without a result payload are released at completion. */
    return async_status( async, FALSE );
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiAsync( IXGameSaveImpl3 *iface, XUserHandle requestingUser, const char *configurationId, XAsyncBlock *async )
{
    FIXME( "iface %p, requestingUser %p, configurationId %s, async %p stub!\n", iface, requestingUser, debugstr_a( configurationId ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetFolderWithUiResult( IXGameSaveImpl3 *iface, XAsyncBlock *async, SIZE_T folderSize, char *folderResult )
{
    FIXME( "iface %p, async %p, folderSize %Iu, folderResult %p stub!\n", iface, async, folderSize, folderResult );
    return E_NOTIMPL;
}

static HRESULT WINAPI x_game_save_XGameSaveFilesGetRemainingQuota( IXGameSaveImpl3 *iface, XUserHandle userContext, const char *configurationId, INT64 *remainingQuota )
{
    FIXME( "iface %p, userContext %p, configurationId %s, remainingQuota %p stub!\n", iface, userContext, debugstr_a( configurationId ), remainingQuota );
    return E_NOTIMPL;
}

static const struct IXGameSaveImpl3Vtbl x_game_save_vtbl =
{
    x_game_save_QueryInterface,
    x_game_save_AddRef,
    x_game_save_Release,
    /* IXGameSaveImpl methods */
    x_game_save_XGameSaveInitializeProvider,
    x_game_save_XGameSaveInitializeProviderAsync,
    x_game_save_XGameSaveInitializeProviderResult,
    x_game_save_XGameSaveCloseProvider,
    x_game_save_XGameSaveGetRemainingQuota,
    x_game_save_XGameSaveGetRemainingQuotaAsync,
    x_game_save_XGameSaveGetRemainingQuotaResult,
    x_game_save_XGameSaveDeleteContainer,
    x_game_save_XGameSaveDeleteContainerAsync,
    x_game_save_XGameSaveDeleteContainerResult,
    x_game_save_XGameSaveGetContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfo,
    x_game_save_XGameSaveEnumerateContainerInfoByName,
    x_game_save_XGameSaveCreateContainer,
    x_game_save_XGameSaveCloseContainer,
    x_game_save_XGameSaveEnumerateBlobInfo,
    x_game_save_XGameSaveEnumerateBlobInfoByName,
    x_game_save_XGameSaveReadBlobData,
    x_game_save_XGameSaveReadBlobDataAsync,
    x_game_save_XGameSaveReadBlobDataResult,
    x_game_save_XGameSaveCreateUpdate,
    x_game_save_XGameSaveCloseUpdate,
    x_game_save_XGameSaveSubmitBlobWrite,
    x_game_save_XGameSaveSubmitBlobDelete,
    x_game_save_XGameSaveSubmitUpdate,
    x_game_save_XGameSaveSubmitUpdateAsync,
    x_game_save_XGameSaveSubmitUpdateResult,
    /* IXGameSaveImpl2 methods */
    x_game_save_XGameSaveFilesGetFolderWithUiAsync,
    x_game_save_XGameSaveFilesGetFolderWithUiResult,
    x_game_save_XGameSaveFilesGetRemainingQuota,
};

static struct x_game_save x_game_save =
{
    {&x_game_save_vtbl},
    0,
};

IXGameSaveImpl *x_game_save_impl = (IXGameSaveImpl *)&x_game_save.IXGameSaveImpl3_iface;
