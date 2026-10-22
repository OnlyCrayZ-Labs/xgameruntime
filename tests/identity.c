// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Title ID and sandbox contract tests. LGPL-2.1-or-later.
 * Usage: identity-tests.exe xgameruntime.dll <expected hex title id | none> [packaged | unpackaged]
 * The title ID and package <Identity> are read from MicrosoftGame.Config beside
 * this executable. The optional third argument adds the XPackage identity checks. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <initguid.h>
#include "xgame.h"
#include "xsystem.h"
#include "xpackage.h"
#include "xgameerr.h"

static LONG checks, failures;
#define check(e) do { InterlockedIncrement(&checks); if (!(e)) { InterlockedIncrement(&failures); printf("FAIL %u: %s\n", __LINE__, #e); } } while (0)

int main(int argc, char **argv)
{
    HMODULE dll;
    HRESULT (WINAPI *initialize)(ULONG, ULONG);
    HRESULT (WINAPI *query)(REFCLSID,REFIID,void **);
    IXGameImpl3 *game;
    IXSystemImpl5 *sys;
    IXPackageImpl3 *package;
    char id1[33], id2[40];
    SIZE_T i;
    UINT32 id = 0xdeadbeef, again = 0;
    char sandbox[16];
    SIZE_T used = 0;
    HRESULT hr;
    if ((argc != 3 && argc != 4) || !(dll = LoadLibraryA(argv[1]))) return 2;
    initialize = (void *)GetProcAddress(dll, "InitializeApiImpl");
    query = (void *)GetProcAddress(dll, "QueryApiImpl");
    if (!initialize || !query) return 2;
    check(initialize(10001, 3193) == S_OK);
    if (FAILED(query(&CLSID_XGameImpl, &IID_IXGameImpl3, (void **)&game)) ||
        FAILED(query(&CLSID_XSystemImpl, &IID_IXSystemImpl5, (void **)&sys))) return 2;

    check(IXGameImpl3_XGameGetXboxTitleId(game, NULL) == E_POINTER);
    hr = IXGameImpl3_XGameGetXboxTitleId(game, &id);
    if (!strcmp(argv[2], "none")) check(FAILED(hr) && !id);
    else check(hr == S_OK && id == strtoul(argv[2], NULL, 16));
    /* The result is stable for the process lifetime. */
    check(IXGameImpl3_XGameGetXboxTitleId(game, &again) == hr && again == id);
    printf("title hr %#lx id %#x\n", hr, id);

    check(IXSystemImpl5_XSystemGetXboxLiveSandboxId(sys, sizeof(sandbox), sandbox, &used) == S_OK);
    check(!strcmp(sandbox, "RETAIL") && used == 7);
    memset(sandbox, 0, sizeof(sandbox));
    check(IXSystemImpl5_XSystemGetXboxLiveSandboxId(sys, sizeof(sandbox), sandbox, NULL) == S_OK);
    check(!strcmp(sandbox, "RETAIL"));
    check(IXSystemImpl5_XSystemGetXboxLiveSandboxId(sys, sizeof(sandbox), NULL, &used) == E_POINTER);
    check(IXSystemImpl5_XSystemGetXboxLiveSandboxId(sys, 4, sandbox, &used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

    if (argc == 4)
    {
        if (FAILED(query(&CLSID_XPackageImpl, &IID_IXPackageImpl3, (void **)&package))) return 2;
        memset(id1, 'x', sizeof(id1));
        check(IXPackageImpl3_XPackageGetCurrentProcessPackageIdentifier(package, sizeof(id1), NULL) == E_INVALIDARG);
        check(IXPackageImpl3_XPackageGetCurrentProcessPackageIdentifier(package, 32, id1) == E_INVALIDARG);
        check(id1[0] == 'x');
        hr = IXPackageImpl3_XPackageGetCurrentProcessPackageIdentifier(package, sizeof(id1), id1);
        if (!strcmp(argv[3], "packaged"))
        {
            check(IXPackageImpl3_XPackageIsPackagedProcess(package));
            check(hr == S_OK && strlen(id1) == 32);
            for (i = 0; i < 32; ++i) check((id1[i] >= '0' && id1[i] <= '9') || (id1[i] >= 'a' && id1[i] <= 'f'));
            /* Fixed for the process lifetime; larger buffers are fine. */
            memset(id2, 'x', sizeof(id2));
            check(IXPackageImpl3_XPackageGetCurrentProcessPackageIdentifier(package, sizeof(id2), id2) == S_OK);
            check(!strcmp(id1, id2) && id2[33] == 'x');
            check(strcmp(id1, "00000000000000000000000000000000"));
        }
        else
        {
            check(!IXPackageImpl3_XPackageIsPackagedProcess(package));
            check(hr == E_GAMEPACKAGE_APP_NOT_PACKAGED && id1[0] == 'x');
            check(IXPackageImpl3_XPackageGetCurrentProcessPackageIdentifier(package, sizeof(id1), id1) == E_GAMEPACKAGE_APP_NOT_PACKAGED);
        }
        printf("package hr %#lx packaged %d\n", hr, IXPackageImpl3_XPackageIsPackagedProcess(package));
        if (hr == S_OK) printf("package id %s\n", id1);
    }
    printf("%ld checks, %ld failures\n", checks, failures);
    return failures ? 1 : 0;
}
