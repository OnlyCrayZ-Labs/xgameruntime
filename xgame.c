// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/*
 * Xbox Game runtime Library
 *  GDK Component: System API -> XGame
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "private.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdkc);

struct x_game
{
    IXGameImpl3 IXGameImpl3_iface;
    LONG ref;
};

static inline struct x_game *impl_from_IXGameImpl3( IXGameImpl3 *iface )
{
    return CONTAINING_RECORD( iface, struct x_game, IXGameImpl3_iface );
}

static HRESULT WINAPI x_game_QueryInterface( IXGameImpl3 *iface, REFIID iid, void **out )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown    ) ||
        IsEqualGUID( iid, &IID_IXGameImpl  ) ||
        IsEqualGUID( iid, &IID_IXGameImpl2 ) ||
        IsEqualGUID( iid, &IID_IXGameImpl3 ))
    {
        IXGameImpl3_AddRef( *out = &impl->IXGameImpl3_iface );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI x_game_AddRef( IXGameImpl3 *iface )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI x_game_Release( IXGameImpl3 *iface )
{
    struct x_game *impl = impl_from_IXGameImpl3( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

/* Title identity comes from the MicrosoftGame.Config shipped beside the title
 * executable, read once: the hexadecimal <TitleId> and the <MSAAppId> used for
 * title-scoped Xbox Live sign-in. Nothing is invented when either is absent. */
static INIT_ONCE config_once = INIT_ONCE_STATIC_INIT;
static HRESULT title_hr, app_hr, store_hr, version_hr, name_hr;
static UINT32 title_id;
static char app_id[65], store_id[17], package_version[24], package_name[51];

static BOOL config_element( const char *text, const char *name, const char **start, const char **end )
{
    char open[32], close[32];
    snprintf( open, sizeof(open), "<%s>", name );
    snprintf( close, sizeof(close), "</%s>", name );
    if (!(*start = strstr( text, open )) || !(*end = strstr( *start += strlen( open ), close ))) return FALSE;
    while (*start < *end && strchr( " \t\r\n", **start )) ++*start;
    while (*end > *start && strchr( " \t\r\n", (*end)[-1] )) --*end;
    return *end > *start;
}

static HRESULT read_title_id( const char *text, UINT32 *id )
{
    const char *start, *end;
    UINT32 value = 0;
    if (!config_element( text, "TitleId", &start, &end ) || end - start > 8) return E_GAMEUSER_NO_TITLE_ID;
    for (; start < end; ++start)
    {
        if (*start >= '0' && *start <= '9') value = value * 16 + (*start - '0');
        else if ((*start | 0x20) >= 'a' && (*start | 0x20) <= 'f') value = value * 16 + ((*start | 0x20) - 'a' + 10);
        else return E_GAMEUSER_NO_TITLE_ID;
    }
    if (!value) return E_GAMEUSER_NO_TITLE_ID;
    *id = value;
    return S_OK;
}

/* Store product ID: 12 upper-case alphanumerics (for example 9P38D19T7LRV). */
static HRESULT read_store_id( const char *text, char *id, SIZE_T size )
{
    const char *start, *end, *c;
    if (!config_element( text, "StoreId", &start, &end ) || end - start >= size) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    for (c = start; c < end; ++c)
        if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z'))) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    memcpy( id, start, end - start );
    id[end - start] = 0;
    return S_OK;
}

/* Attribute of the <Identity> element; values are limited to the characters
 * package names and versions may contain (alphanumerics, '.' and '-'). */
static HRESULT read_identity( const char *text, const char *attribute, char *value, SIZE_T size )
{
    const char *identity, *end, *start, *c;
    SIZE_T length = strlen( attribute );
    if (!(identity = strstr( text, "<Identity " )) || !(end = strchr( identity, '>' ))) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    for (start = identity; (start = strstr( start, attribute )) && start < end; ++start)
        if (strchr( " \t\r\n", start[-1] ) && start[length] == '=' && start[length + 1] == '"') break;
    if (!start || start >= end) return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
    start += length + 2;
    for (c = start; c < end && *c != '"'; ++c)
        if (!((*c >= '0' && *c <= '9') || ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'z') || *c == '.' || *c == '-')) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    if (c == end || c == start || (SIZE_T)(c - start) >= size) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    memcpy( value, start, c - start );
    value[c - start] = 0;
    return S_OK;
}

/* Installed package version: the Version attribute of <Identity>, "a.b.c.d". */
static HRESULT read_package_version( const char *text, char *version, SIZE_T size )
{
    HRESULT hr;
    const char *c;
    if (FAILED(hr = read_identity( text, "Version", version, size ))) return hr;
    for (c = version; *c; ++c)
        if (!((*c >= '0' && *c <= '9') || *c == '.')) return HRESULT_FROM_WIN32( ERROR_INVALID_DATA );
    return S_OK;
}

static HRESULT read_app_id( const char *text, char *id, SIZE_T size )
{
    const char *start, *end, *c;
    if (!config_element( text, "MSAAppId", &start, &end ) || end - start >= size) return E_GAMEUSER_NO_MSAAPPID;
    for (c = start; c < end; ++c)
        if (!((*c >= '0' && *c <= '9') || ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'z') || *c == '-')) return E_GAMEUSER_MALFORMED_MSAAPPID;
    memcpy( id, start, end - start );
    id[end - start] = 0;
    return S_OK;
}

static HRESULT read_config( char **text )
{
    WCHAR path[MAX_PATH + 32], *slash;
    DWORD length, size, read;
    HANDLE file;
    HRESULT hr = S_OK;

    length = GetModuleFileNameW( NULL, path, MAX_PATH );
    if (!length || length >= MAX_PATH || !(slash = wcsrchr( path, '\\' ))) return HRESULT_FROM_WIN32( ERROR_FILE_NOT_FOUND );
    wcscpy( slash + 1, L"MicrosoftGame.Config" );
    file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
    if (file == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32( GetLastError() );
    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE || size > 1024 * 1024) hr = HRESULT_FROM_WIN32( ERROR_FILE_TOO_LARGE );
    else if (!(*text = malloc( size + 1 ))) hr = E_OUTOFMEMORY;
    else if (!ReadFile( file, *text, size, &read, NULL ) || read != size)
    {
        hr = HRESULT_FROM_WIN32( GetLastError() );
        free( *text );
    }
    else (*text)[size] = 0;
    CloseHandle( file );
    return hr;
}

static BOOL WINAPI config_init( INIT_ONCE *once, void *param, void **context )
{
    char *text;
    if (FAILED(title_hr = app_hr = store_hr = version_hr = name_hr = read_config( &text ))) return TRUE;
    version_hr = read_package_version( text, package_version, sizeof(package_version) );
    name_hr = read_identity( text, "Name", package_name, sizeof(package_name) );
    title_hr = read_title_id( text, &title_id );
    app_hr = read_app_id( text, app_id, sizeof(app_id) );
    store_hr = read_store_id( text, store_id, sizeof(store_id) );
    free( text );
    return TRUE;
}

HRESULT game_identity( UINT32 *title, char *app, SIZE_T app_size )
{
    InitOnceExecuteOnce( &config_once, config_init, NULL, NULL );
    if (FAILED(title_hr)) return title_hr;
    if (FAILED(app_hr)) return app_hr;
    if (strlen( app_id ) >= app_size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    *title = title_id;
    strcpy( app, app_id );
    return S_OK;
}

HRESULT game_package_version( char *version, SIZE_T size )
{
    InitOnceExecuteOnce( &config_once, config_init, NULL, NULL );
    if (FAILED(version_hr)) return version_hr;
    if (strlen( package_version ) >= size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    strcpy( version, package_version );
    return S_OK;
}

HRESULT game_package_name( char *name, SIZE_T size )
{
    InitOnceExecuteOnce( &config_once, config_init, NULL, NULL );
    if (FAILED(name_hr)) return name_hr;
    if (strlen( package_name ) >= size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    strcpy( name, package_name );
    return S_OK;
}

HRESULT game_store_id( char *id, SIZE_T size )
{
    InitOnceExecuteOnce( &config_once, config_init, NULL, NULL );
    if (FAILED(store_hr)) return store_hr;
    if (strlen( store_id ) >= size) return HRESULT_FROM_WIN32( ERROR_INSUFFICIENT_BUFFER );
    strcpy( id, store_id );
    return S_OK;
}

static HRESULT WINAPI x_game_XGameGetXboxTitleId( IXGameImpl3 *iface, UINT32 *titleId )
{
    if (!titleId) return E_POINTER;
    InitOnceExecuteOnce( &config_once, config_init, NULL, NULL );
    *titleId = SUCCEEDED(title_hr) ? title_id : 0;
    TRACE( "hr %#lx title %#x.\n", title_hr, *titleId );
    return title_hr;
}

static void WINAPI x_game_XLaunchNewGame( IXGameImpl3 *iface, const char *exePath, const char *args, XUserHandle defaultUser )
{
    FIXME( "iface %p exePath %s, args %s, defaultUser %p stub!\n", iface, debugstr_a( exePath ), debugstr_a( args ), defaultUser );
}

static HRESULT WINAPI x_game_XLaunchRestartOnCrash( IXGameImpl3 *iface, const char *args, UINT32 reserved )
{
    FIXME( "iface %p, args %s, reserved %u stub!\n", iface, debugstr_a( args ), reserved );
    return E_NOTIMPL;
}

static const struct IXGameImpl3Vtbl x_game_vtbl =
{
    x_game_QueryInterface,
    x_game_AddRef,
    x_game_Release,
    /* IXGameImpl methods */
    x_game_XGameGetXboxTitleId,
    /* IXGameImpl2 methods */
    x_game_XLaunchNewGame,
    /* IXGameImpl3 methods */
    x_game_XLaunchRestartOnCrash,
};

static struct x_game x_game =
{
    {&x_game_vtbl},
    0,
};

IXGameImpl *x_game_impl = (IXGameImpl *)&x_game.IXGameImpl3_iface;
