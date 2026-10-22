// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Title profile, privilege and token contract tests against a synthetic service.
 * Copyright 2026 Xodus contributors. LGPL-2.1-or-later.
 * Usage: xbl-tests.exe xgameruntime.dll <success|profile-fail|token-fail>
 * MicrosoftGame.Config must sit beside this executable (see xbl-fixture.py). */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <initguid.h>
#include "xasyncprovider.h"
#include "xuser.h"
#include "xnetworking.h"
#include "xstore.h"
#include "xgamesave.h"
#include "xgameerr.h"

static IXThreadingImpl *api;
static IXUserImpl6 *user;
static IXUserGamertagImpl *gamertags;
static IXNetworkingImpl2 *net;
static IXStoreImpl6 *store;
static IXGameSaveImpl3 *saves;
static LONG checks, failures;
#define check(e) do { ++checks; if (!(e)) { ++failures; printf("FAIL %u: %s\n",__LINE__,#e); } } while(0)
#define status(a) IXThreadingImpl_XAsyncGetStatus(api,a,TRUE)
/* Year 9999 saturates where time_t is 32-bit (i386 builds). */
#define FAR_DATE (sizeof(time_t) == 8 ? (time_t)253402300799ull : (time_t)0x7fffffff)

static XUserHandle add_user(void)
{
    XAsyncBlock a = {0};
    XUserHandle u = NULL;
    check(IXUserImpl6_XUserAddAsync(user,XUserAddOptions_AddDefaultUserSilently,&a) == S_OK);
    check(status(&a) == S_OK);
    check(IXUserImpl6_XUserAddResult(user,&a,&u) == S_OK && u);
    return u;
}

static void gamertag(XUserHandle u, XUserGamertagComponent component, const char *expected, HRESULT hr)
{
    char buffer[128] = "unchanged";
    SIZE_T used = 99;
    check(IXUserGamertagImpl_XUserGetGamertag(gamertags,u,component,sizeof(buffer),buffer,&used) == hr);
    if (SUCCEEDED(hr)) check(!strcmp(buffer,expected) && used == strlen(expected) + 1);
    else check(!used);
}

static void profile(XUserHandle u, HRESULT expected)
{
    BOOLEAN has = TRUE;
    XUserPrivilegeDenyReason reason = 99;
    char tiny[4];
    SIZE_T used = 99;
    if (FAILED(expected))
    {
        gamertag(u,XUserGamertagComponent_Classic,NULL,expected);
        check(IXUserImpl6_XUserCheckPrivilege(user,u,XUserPrivilegeOptions_None,XUserPrivilege_Multiplayer,&has,&reason) == expected);
        check(!has);
        return;
    }
    gamertag(u,XUserGamertagComponent_Classic,"Classic",S_OK);
    gamertag(u,XUserGamertagComponent_Modern,"Modern",S_OK);
    gamertag(u,XUserGamertagComponent_ModernSuffix,"1234",S_OK);
    gamertag(u,XUserGamertagComponent_UniqueModern,"Modern#1234",S_OK);
    check(IXUserGamertagImpl_XUserGetGamertag(gamertags,u,XUserGamertagComponent_Classic,sizeof(tiny),tiny,&used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && !used);
    check(IXUserGamertagImpl_XUserGetGamertag(gamertags,u,4,sizeof(tiny),tiny,&used) == E_INVALIDARG);
    check(IXUserGamertagImpl_XUserGetGamertag(gamertags,u,XUserGamertagComponent_Classic,sizeof(tiny),NULL,&used) == E_POINTER);
    check(IXUserGamertagImpl_XUserGetGamertag(gamertags,(XUserHandle)1,XUserGamertagComponent_Classic,sizeof(tiny),tiny,&used) == E_GAMERUNTIME_INVALID_HANDLE);
    check(IXUserImpl6_XUserCheckPrivilege(user,u,XUserPrivilegeOptions_None,XUserPrivilege_Multiplayer,&has,&reason) == S_OK);
    check(has && reason == XUserPrivilegeDenyReason_None);
    check(IXUserImpl6_XUserCheckPrivilege(user,u,XUserPrivilegeOptions_None,XUserPrivilege_CrossPlay,&has,NULL) == S_OK && has);
    check(IXUserImpl6_XUserCheckPrivilege(user,u,XUserPrivilegeOptions_None,XUserPrivilege_Sessions,&has,&reason) == S_OK);
    check(!has && reason == XUserPrivilegeDenyReason_Restricted);
    check(IXUserImpl6_XUserCheckPrivilege(user,NULL,XUserPrivilegeOptions_AllUsers,XUserPrivilege_Multiplayer,&has,&reason) == S_OK && has);
    check(IXUserImpl6_XUserCheckPrivilege(user,u,4,XUserPrivilege_Multiplayer,&has,&reason) == E_INVALIDARG);
    check(IXUserImpl6_XUserCheckPrivilege(user,u,XUserPrivilegeOptions_None,XUserPrivilege_Multiplayer,NULL,&reason) == E_POINTER);
    check(IXUserImpl6_XUserCheckPrivilege(user,(XUserHandle)1,XUserPrivilegeOptions_None,XUserPrivilege_Multiplayer,&has,&reason) == E_GAMERUNTIME_INVALID_HANDLE);
}

static void tokens(XUserHandle u, HRESULT expected)
{
    static const char body[] = "{\"x\":1}";
    XUserGetTokenAndSignatureHttpHeader header = {"Content-Type","application/json"};
    XUserGetTokenAndSignatureData *data = NULL;
    XUserGetTokenAndSignatureUtf16Data *data16 = NULL;
    XAsyncBlock a = {0}, b = {0};
    SIZE_T size = 0, used = 0;
    char *buffer;
    check(IXUserImpl6_XUserGetTokenAndSignatureAsync(user,u,XUserGetTokenAndSignatureOptions_None,"POST",NULL,0,NULL,0,NULL,&a) == E_INVALIDARG);
    check(IXUserImpl6_XUserGetTokenAndSignatureAsync(user,u,4,"POST","https://a.xboxlive.com/",0,NULL,0,NULL,&a) == E_INVALIDARG);
    check(IXUserImpl6_XUserGetTokenAndSignatureAsync(user,(XUserHandle)1,0,"POST","https://a.xboxlive.com/",0,NULL,0,NULL,&a) == E_GAMERUNTIME_INVALID_HANDLE);
    check(IXUserImpl6_XUserGetTokenAndSignatureAsync(user,u,XUserGetTokenAndSignatureOptions_ForceRefresh,"POST",
          "https://title.xboxlive.com/v1/x?y=1",1,&header,sizeof(body)-1,body,&a) == S_OK);
    check(status(&a) == expected);
    if (FAILED(expected))
    {
        check(IXUserImpl6_XUserGetTokenAndSignatureResult(user,&a,0,NULL,&data,&used) == expected && !data);
        return;
    }
    check(IXUserImpl6_XUserGetTokenAndSignatureResultSize(user,&a,&size) == S_OK);
    check(size == sizeof(*data) + sizeof("XBL3.0 x=hash;tok") + sizeof("sig=="));
    buffer = malloc(size);
    check(IXUserImpl6_XUserGetTokenAndSignatureUtf16Result(user,&a,size,buffer,&data16,&used) == E_INVALIDARG);
    check(IXUserImpl6_XUserGetTokenAndSignatureResult(user,&a,size - 1,buffer,&data,&used) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    check(IXUserImpl6_XUserGetTokenAndSignatureResult(user,&a,size,buffer,&data,&used) == S_OK);
    check(data == (void *)buffer && used == size);
    check(!strcmp(data->token,"XBL3.0 x=hash;tok") && data->tokenSize == sizeof("XBL3.0 x=hash;tok"));
    check(data->signature && !strcmp(data->signature,"sig==") && data->signatureSize == sizeof("sig=="));
    free(buffer);
    /* Non-Xbox Live URLs get an empty token and no signature (UTF-16 API). */
    check(IXUserImpl6_XUserGetTokenAndSignatureUtf16Async(user,u,XUserGetTokenAndSignatureOptions_None,L"GET",
          L"https://example.com/",0,NULL,0,NULL,&b) == S_OK);
    check(status(&b) == S_OK);
    check(IXUserImpl6_XUserGetTokenAndSignatureUtf16ResultSize(user,&b,&size) == S_OK && size == sizeof(*data16) + sizeof(WCHAR));
    buffer = malloc(size);
    check(IXUserImpl6_XUserGetTokenAndSignatureUtf16Result(user,&b,size,buffer,&data16,&used) == S_OK);
    check(data16 && !data16->token[0] && data16->tokenCount == 1 && !data16->signature && !data16->signatureCount);
    free(buffer);
}

static void security(HRESULT expected)
{
    XNetworkingSecurityInformation *info = NULL;
    XAsyncBlock a = {0}, b = {0};
    SIZE_T size = 0, used = 0;
    void *buffer;
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlAsync(net,NULL,&a) == E_INVALIDARG);
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlAsync(net,"https://settings.xboxlive.com/s?x=1",&a) == S_OK);
    check(status(&a) == expected);
    if (FAILED(expected))
    {
        check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlAsyncResult(net,&a,0,&used,NULL,&info) == expected && !info);
        return;
    }
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlAsyncResultSize(net,&a,&size) == S_OK);
    check(size == sizeof(*info) + 2 * sizeof(XNetworkingThumbprint) + 40);
    buffer = malloc(size);
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlAsyncResult(net,&a,size,&used,buffer,&info) == S_OK);
    check(info == buffer && used == size);
    check(info->enabledHttpSecurityProtocolFlags == 0x2800 && info->thumbprintCount == 2);
    check(info->thumbprints[0].thumbprintType == XNetworkingThumbprintType_Issuer && info->thumbprints[0].thumbprintBufferByteCount == 20);
    check(info->thumbprints[0].thumbprintBuffer[0] == 0x54 && info->thumbprints[0].thumbprintBuffer[19] == 0x2d);
    check(info->thumbprints[1].thumbprintType == XNetworkingThumbprintType_Leaf && info->thumbprints[1].thumbprintBuffer[0] == 0xd5);
    /* Pinned verification is reported as unimplemented, never silently accepted. */
    check(IXNetworkingImpl2_XNetworkingVerifyServerCertificate(net,(void *)1,info) == E_NOTIMPL);
    free(buffer);
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlUtf16Async(net,L"https://example.com/",&b) == S_OK);
    check(status(&b) == S_OK);
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize(net,&b,&size) == S_OK && size == sizeof(*info));
    buffer = malloc(size);
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlAsyncResult(net,&b,size,&used,buffer,&info) == E_INVALIDARG);
    check(IXNetworkingImpl2_XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult(net,&b,size,&used,buffer,&info) == S_OK);
    check(info->enabledHttpSecurityProtocolFlags == 0x2800 && !info->thumbprintCount && !info->thumbprints);
    check(IXNetworkingImpl2_XNetworkingVerifyServerCertificate(net,(void *)1,info) == S_OK);
    check(IXNetworkingImpl2_XNetworkingVerifyServerCertificate(net,NULL,info) == E_INVALIDARG);
    check(IXNetworkingImpl2_XNetworkingVerifyServerCertificate(net,(void *)1,NULL) == E_INVALIDARG);
    free(buffer);
}

static void CALLBACK license_changed(void *context) {}
static void store_context(XUserHandle u)
{
    XStoreContextHandle c = (void *)1, d = (void *)1, bad = (void *)1;
    XTaskQueueRegistrationToken t = {0};
    XAsyncBlock a = {0};
    check(IXStoreImpl6_XStoreCreateContext(store,u,NULL) == E_POINTER);
    check(IXStoreImpl6_XStoreCreateContext(store,(XUserHandle)1,&bad) == E_GAMERUNTIME_INVALID_HANDLE && !bad);
    check(IXStoreImpl6_XStoreCreateContext(store,u,&c) == S_OK && c);
    check(IXStoreImpl6_XStoreCreateContext(store,NULL,&d) == S_OK && d && d != c);
    check(IXStoreImpl6_XStoreRegisterGameLicenseChanged(store,(void *)1,NULL,NULL,license_changed,&t) == E_INVALIDARG && !t.token);
    check(IXStoreImpl6_XStoreRegisterGameLicenseChanged(store,c,NULL,NULL,NULL,&t) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreRegisterGameLicenseChanged(store,c,NULL,NULL,license_changed,&t) == S_OK && t.token);
    check(IXStoreImpl6_XStoreUnregisterGameLicenseChanged(store,c,t,TRUE));
    /* Unimplemented store operations still say so. */
    check(IXStoreImpl6_XStoreQueryAddOnLicensesAsync(store,c,&a) == E_NOTIMPL);
    IXStoreImpl6_XStoreCloseContextHandle(store,c);
    IXStoreImpl6_XStoreCloseContextHandle(store,d);
    IXStoreImpl6_XStoreCloseContextHandle(store,(void *)1);
    check(IXStoreImpl6_XStoreRegisterGameLicenseChanged(store,c,NULL,NULL,license_changed,&t) == E_INVALIDARG);
}

struct seen { unsigned int count; const char *ids[4]; BOOLEAN stop_after_first; };
static BOOLEAN CALLBACK seen_product(const XStoreProduct *p, void *context)
{
    struct seen *s = context;
    if (s->count < 4) s->ids[s->count] = p->storeId;
    ++s->count;
    return !s->stop_after_first;
}
static void store_queries(XUserHandle u, HRESULT expected)
{
    XStoreContextHandle c = NULL;
    XStoreGameLicense license;
    XStoreProductQueryHandle q = NULL, next = NULL;
    XAsyncBlock a = {0}, b = {0}, n = {0};
    struct seen seen = {0};
    check(IXStoreImpl6_XStoreCreateContext(store,u,&c) == S_OK);
    check(IXStoreImpl6_XStoreQueryGameLicenseAsync(store,NULL,&a) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryGameLicenseAsync(store,c,&a) == S_OK);
    check(status(&a) == expected);
    check(IXStoreImpl6_XStoreQueryGameLicenseResult(store,&a,&license) == expected);
    if (SUCCEEDED(expected))
    {
        check(!strcmp(license.skuStoreId,"9P38D19T7LRV/0010") && license.isActive && !license.isTrial && !license.isDiscLicense);
        check(license.expirationDate == FAR_DATE && !license.trialTimeRemainingInSeconds);
    }
    check(IXStoreImpl6_XStoreQueryEntitledProductsAsync(store,c,XStoreProductKind_Game | XStoreProductKind_Durable,0,&b) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryEntitledProductsAsync(store,c,0x100,2,&b) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryEntitledProductsAsync(store,c,XStoreProductKind_Game | XStoreProductKind_Durable,2,&b) == S_OK);
    check(status(&b) == expected);
    check(IXStoreImpl6_XStoreQueryEntitledProductsResult(store,&b,&q) == expected);
    if (FAILED(expected)) { check(!q); IXStoreImpl6_XStoreCloseContextHandle(store,c); return; }
    check(q != NULL);
    check(IXStoreImpl6_XStoreEnumerateProductsQuery(store,q,&seen,seen_product) == S_OK);
    check(seen.count == 2 && !strcmp(seen.ids[0],"9N7J4DKK7V15") && !strcmp(seen.ids[1],"9ADDON000001"));
    check(IXStoreImpl6_XStoreProductsQueryHasMorePages(store,q));
    memset(&seen,0,sizeof(seen)); seen.stop_after_first = TRUE;
    check(IXStoreImpl6_XStoreEnumerateProductsQuery(store,q,&seen,seen_product) == S_OK && seen.count == 1);
    check(IXStoreImpl6_XStoreProductsQueryNextPageAsync(store,q,&n) == S_OK);
    check(status(&n) == S_OK);
    check(IXStoreImpl6_XStoreProductsQueryNextPageResult(store,&n,&next) == S_OK && next && next != q);
    memset(&seen,0,sizeof(seen));
    check(IXStoreImpl6_XStoreEnumerateProductsQuery(store,next,&seen,seen_product) == S_OK);
    check(seen.count == 1 && !strcmp(seen.ids[0],"9ADDON000002"));
    check(!IXStoreImpl6_XStoreProductsQueryHasMorePages(store,next));
    memset(&n,0,sizeof(n));
    check(IXStoreImpl6_XStoreProductsQueryNextPageAsync(store,next,&n) == E_INVALIDARG);
    /* Product contents survive closing the other handle of the same set. */
    IXStoreImpl6_XStoreCloseProductsQueryHandle(store,next);
    memset(&seen,0,sizeof(seen)); seen.stop_after_first = TRUE;
    check(IXStoreImpl6_XStoreEnumerateProductsQuery(store,q,&seen,seen_product) == S_OK && seen.count == 1);
    IXStoreImpl6_XStoreCloseProductsQueryHandle(store,q);
    check(IXStoreImpl6_XStoreEnumerateProductsQuery(store,q,&seen,seen_product) == E_INVALIDARG);
    check(!IXStoreImpl6_XStoreProductsQueryHasMorePages(store,q));
    IXStoreImpl6_XStoreCloseContextHandle(store,c);
}
static const XStoreProduct *captured_product;
static BOOLEAN CALLBACK capture_product(const XStoreProduct *p, void *context)
{
    if (!strcmp(p->storeId,"9ADDON000001")) captured_product = p;
    return TRUE;
}
static void store_product_fields(XUserHandle u)
{
    XStoreContextHandle c = NULL;
    XStoreProductQueryHandle q = NULL;
    XAsyncBlock b = {0};
    const XStoreProduct *p;
    check(IXStoreImpl6_XStoreCreateContext(store,u,&c) == S_OK);
    check(IXStoreImpl6_XStoreQueryEntitledProductsAsync(store,c,XStoreProductKind_Game | XStoreProductKind_Durable,10,&b) == S_OK);
    check(status(&b) == S_OK && IXStoreImpl6_XStoreQueryEntitledProductsResult(store,&b,&q) == S_OK);
    check(IXStoreImpl6_XStoreEnumerateProductsQuery(store,q,NULL,capture_product) == S_OK);
    p = captured_product;
    check(p != NULL);
    if (p)
    {
        check(!strcmp(p->title,"Aircraft Pack") && !strcmp(p->description,"Add-on") && !strcmp(p->language,"en-us"));
        check(p->productKind == XStoreProductKind_Durable && p->hasDigitalDownload && p->isInUserCollection);
        check(!strcmp(p->inAppOfferToken,"pack1") && p->linkUri && !p->linkUri[0]);
        check(p->price.currencyCode && !p->price.price && !p->keywordsCount && !p->imagesCount && !p->videosCount);
        check(p->skusCount == 1 && !strcmp(p->skus[0].skuId,"0010") && !strcmp(p->skus[0].title,"Pack"));
        check(p->skus[0].isTrial && p->skus[0].isInUserCollection && !p->skus[0].isSubscription);
        check(p->skus[0].collectionData.isTrial && p->skus[0].collectionData.trialTimeRemainingInSeconds == 3600);
        check(p->skus[0].collectionData.quantity == 1 && !strcmp(p->skus[0].collectionData.developerOfferId,"dev1"));
        check(p->skus[0].collectionData.startDate == 1700000000 && p->skus[0].collectionData.endDate == FAR_DATE);
    }
    IXStoreImpl6_XStoreCloseProductsQueryHandle(store,q);
    IXStoreImpl6_XStoreCloseContextHandle(store,c);
}

static void store_license_token(XUserHandle u, HRESULT expected)
{
    static const char *ids[] = {"9ADDON000001","9ADDON000002"};
    static const char *bad[] = {"9ADDON000001",NULL};
    XStoreContextHandle c = NULL;
    XAsyncBlock a = {0};
    SIZE_T size = 0;
    char token[64], tiny[4];
    check(IXStoreImpl6_XStoreCreateContext(store,u,&c) == S_OK);
    check(IXStoreImpl6_XStoreQueryLicenseTokenAsync(store,c,ids,2,NULL,&a) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryLicenseTokenAsync(store,c,bad,2,"nonce-42",&a) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryLicenseTokenAsync(store,(void *)1,ids,2,"nonce-42",&a) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryLicenseTokenAsync(store,c,ids,2,"nonce-42",&a) == S_OK);
    check(status(&a) == expected);
    if (FAILED(expected))
    {
        check(IXStoreImpl6_XStoreQueryLicenseTokenResult(store,&a,sizeof(token),token) == expected);
        IXStoreImpl6_XStoreCloseContextHandle(store,c);
        return;
    }
    check(IXStoreImpl6_XStoreQueryLicenseTokenResultSize(store,&a,&size) == S_OK && size == sizeof("eyJsaWNlbnNl.synthetic.token"));
    check(IXStoreImpl6_XStoreQueryLicenseTokenResult(store,&a,sizeof(tiny),tiny) == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    check(IXStoreImpl6_XStoreQueryLicenseTokenResult(store,&a,sizeof(token),token) == S_OK);
    check(!strcmp(token,"eyJsaWNlbnNl.synthetic.token"));
    IXStoreImpl6_XStoreCloseContextHandle(store,c);
}

/* Local XGameSave provider: persistence, atomic updates, limits, async forms. */
#define SCID "00000000-0000-0000-0000-000060000000"
struct infos { unsigned int count; char name[64], display[64]; UINT32 blobs; UINT64 size; BOOLEAN sync; time_t modified; };
static BOOLEAN CALLBACK container_info(const XGameSaveContainerInfo *info, void *context)
{
    struct infos *i = context;
    if (!i->count++)
    {
        lstrcpynA(i->name, info->name, sizeof(i->name));
        lstrcpynA(i->display, info->displayName, sizeof(i->display));
        i->blobs = info->blobCount; i->size = info->totalSize; i->sync = info->needsSync; i->modified = info->lastModifiedTime;
    }
    return TRUE;
}
static BOOLEAN CALLBACK blob_info(const XGameSaveBlobInfo *info, void *context)
{
    ++*(unsigned int *)context;
    return TRUE;
}
static XGameSaveProviderHandle open_provider(XUserHandle u)
{
    XGameSaveProviderHandle p = NULL;
    check(IXGameSaveImpl3_XGameSaveInitializeProvider(saves,u,SCID,FALSE,&p) == S_OK && p);
    return p;
}
static void gamesave(XUserHandle u)
{
    static BYTE first[100], big[16 * 1024 * 1024 + 1];
    const char *names[] = {"b"}, *missing[] = {"nope"};
    XGameSaveProviderHandle p, q = NULL;
    XGameSaveContainerHandle c = NULL, c2 = NULL;
    XGameSaveUpdateHandle up = NULL;
    XGameSaveBlob *blobs;
    XAsyncBlock a = {0};
    struct infos infos = {0};
    unsigned int count = 0, i;
    UINT32 n;
    INT64 quota = 0;
    SIZE_T size = 0;
    char buffer[2 * sizeof(XGameSaveBlob) + 100];
    for (i = 0; i < sizeof(first); ++i) first[i] = (BYTE)i;
    check(IXGameSaveImpl3_XGameSaveInitializeProvider(saves,u,SCID,FALSE,NULL) == E_POINTER);
    check(IXGameSaveImpl3_XGameSaveInitializeProvider(saves,u,"bad scid!",FALSE,&q) == E_INVALIDARG && !q);
    check(IXGameSaveImpl3_XGameSaveInitializeProvider(saves,(XUserHandle)1,SCID,FALSE,&q) == E_GAMERUNTIME_INVALID_HANDLE && !q);
    p = open_provider(u);
    check(IXGameSaveImpl3_XGameSaveGetRemainingQuota(saves,p,&quota) == S_OK && quota == 256 * 1024 * 1024);
    check(IXGameSaveImpl3_XGameSaveGetContainerInfo(saves,p,"slot/1",&infos,container_info) == S_OK && !infos.count);
    check(IXGameSaveImpl3_XGameSaveCreateContainer(saves,p,"bad name",&c) == E_GS_INVALID_CONTAINER_NAME && !c);
    check(IXGameSaveImpl3_XGameSaveCreateContainer(saves,p,"slot/1",&c) == S_OK && c);
    /* Reading a container that was never written yields nothing. */
    n = 0;
    check(IXGameSaveImpl3_XGameSaveReadBlobData(saves,c,NULL,&n,0,NULL) == S_OK && !n);
    check(IXGameSaveImpl3_XGameSaveCreateUpdate(saves,c,"Save 1",&up) == S_OK && up);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobWrite(saves,up,"a",first,sizeof(first)) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobWrite(saves,up,"b",NULL,0) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobWrite(saves,up,"x",big,sizeof(big)) == E_GS_UPDATE_TOO_BIG);
    check(IXGameSaveImpl3_XGameSaveSubmitUpdate(saves,up) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitUpdate(saves,up) == E_INVALIDARG);
    IXGameSaveImpl3_XGameSaveCloseUpdate(saves,up);
    check(IXGameSaveImpl3_XGameSaveEnumerateContainerInfo(saves,p,&infos,container_info) == S_OK);
    check(infos.count == 1 && !strcmp(infos.name,"slot/1") && !strcmp(infos.display,"Save 1"));
    check(infos.blobs == 2 && infos.size == 100 && !infos.sync && infos.modified > 1700000000);
    check(IXGameSaveImpl3_XGameSaveGetRemainingQuota(saves,p,&quota) == S_OK && quota == 256 * 1024 * 1024 - 100);
    /* Read everything: blob array then data, names owned by the container. */
    n = 0;
    check(IXGameSaveImpl3_XGameSaveReadBlobData(saves,c,NULL,&n,sizeof(buffer) - 1,(XGameSaveBlob *)buffer) == E_GS_PROVIDED_BUFFER_TOO_SMALL);
    n = 0;
    check(IXGameSaveImpl3_XGameSaveReadBlobData(saves,c,NULL,&n,sizeof(buffer),(XGameSaveBlob *)buffer) == S_OK && n == 2);
    blobs = (XGameSaveBlob *)buffer;
    check(!strcmp(blobs[0].info.name,"a") && blobs[0].info.size == 100 && !memcmp(blobs[0].data,first,100));
    check(!strcmp(blobs[1].info.name,"b") && !blobs[1].info.size);
    n = 1;
    check(IXGameSaveImpl3_XGameSaveReadBlobData(saves,c,names,&n,sizeof(buffer),(XGameSaveBlob *)buffer) == S_OK && n == 1);
    check(!strcmp(((XGameSaveBlob *)buffer)->info.name,"b"));
    n = 1;
    check(IXGameSaveImpl3_XGameSaveReadBlobData(saves,c,missing,&n,sizeof(buffer),(XGameSaveBlob *)buffer) == E_GS_BLOB_NOT_FOUND);
    count = 0;
    check(IXGameSaveImpl3_XGameSaveEnumerateBlobInfo(saves,c,&count,blob_info) == S_OK && count == 2);
    count = 0;
    check(IXGameSaveImpl3_XGameSaveEnumerateBlobInfoByName(saves,c,"a",&count,blob_info) == S_OK && count == 1);
    /* Atomicity: a failing delete leaves the container untouched. */
    check(IXGameSaveImpl3_XGameSaveCreateUpdate(saves,c,NULL,&up) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobWrite(saves,up,"c",first,10) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobDelete(saves,up,"nope") == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitUpdate(saves,up) == E_GS_BLOB_NOT_FOUND);
    IXGameSaveImpl3_XGameSaveCloseUpdate(saves,up);
    count = 0;
    check(IXGameSaveImpl3_XGameSaveEnumerateBlobInfo(saves,c,&count,blob_info) == S_OK && count == 2);
    /* Async update replacing a blob and deleting another, keeping the display name. */
    check(IXGameSaveImpl3_XGameSaveCreateUpdate(saves,c,NULL,&up) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobWrite(saves,up,"a",first,50) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobDelete(saves,up,"b") == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitUpdateAsync(saves,up,&a) == S_OK);
    check(status(&a) == S_OK && IXGameSaveImpl3_XGameSaveSubmitUpdateResult(saves,&a) == S_OK);
    IXGameSaveImpl3_XGameSaveCloseUpdate(saves,up);
    /* Async read. */
    memset(&a,0,sizeof(a));
    check(IXGameSaveImpl3_XGameSaveReadBlobDataAsync(saves,c,NULL,0,&a) == S_OK);
    check(status(&a) == S_OK);
    check(IXThreadingImpl_XAsyncGetResultSize(api,&a,&size) == S_OK && size == sizeof(XGameSaveBlob) + 50);
    check(IXGameSaveImpl3_XGameSaveReadBlobDataResult(saves,&a,sizeof(buffer),(XGameSaveBlob *)buffer,&n) == S_OK && n == 1);
    check(((XGameSaveBlob *)buffer)->info.size == 50 && !memcmp(((XGameSaveBlob *)buffer)->data,first,50));
    /* Persistence across providers (async init), container handles independent. */
    IXGameSaveImpl3_XGameSaveCloseContainer(saves,c);
    IXGameSaveImpl3_XGameSaveCloseProvider(saves,p);
    memset(&a,0,sizeof(a));
    check(IXGameSaveImpl3_XGameSaveInitializeProviderAsync(saves,u,SCID,TRUE,&a) == S_OK);
    check(status(&a) == S_OK && IXGameSaveImpl3_XGameSaveInitializeProviderResult(saves,&a,&q) == S_OK && q);
    memset(&infos,0,sizeof(infos));
    check(IXGameSaveImpl3_XGameSaveGetContainerInfo(saves,q,"slot/1",&infos,container_info) == S_OK);
    check(infos.count == 1 && infos.blobs == 1 && infos.size == 50 && !strcmp(infos.display,"Save 1"));
    check(IXGameSaveImpl3_XGameSaveCreateContainer(saves,q,"slot/2",&c2) == S_OK);
    check(IXGameSaveImpl3_XGameSaveCreateUpdate(saves,c2,"Two",&up) == S_OK);
    check(IXGameSaveImpl3_XGameSaveSubmitBlobWrite(saves,up,"z",first,1) == S_OK && IXGameSaveImpl3_XGameSaveSubmitUpdate(saves,up) == S_OK);
    IXGameSaveImpl3_XGameSaveCloseUpdate(saves,up);
    memset(&infos,0,sizeof(infos));
    check(IXGameSaveImpl3_XGameSaveEnumerateContainerInfoByName(saves,q,"slot/2",&infos,container_info) == S_OK && infos.count == 1 && !strcmp(infos.name,"slot/2"));
    memset(&infos,0,sizeof(infos));
    check(IXGameSaveImpl3_XGameSaveEnumerateContainerInfo(saves,q,&infos,container_info) == S_OK && infos.count == 2);
    /* Delete (sync and async); deleting an absent container is not an error. */
    check(IXGameSaveImpl3_XGameSaveDeleteContainer(saves,q,"slot/2") == S_OK);
    check(IXGameSaveImpl3_XGameSaveDeleteContainer(saves,q,"slot/2") == S_OK);
    memset(&a,0,sizeof(a));
    check(IXGameSaveImpl3_XGameSaveDeleteContainerAsync(saves,q,"slot/1",&a) == S_OK);
    check(status(&a) == S_OK && IXGameSaveImpl3_XGameSaveDeleteContainerResult(saves,&a) == S_OK);
    memset(&infos,0,sizeof(infos));
    check(IXGameSaveImpl3_XGameSaveEnumerateContainerInfo(saves,q,&infos,container_info) == S_OK && !infos.count);
    memset(&a,0,sizeof(a));
    check(IXGameSaveImpl3_XGameSaveGetRemainingQuotaAsync(saves,q,&a) == S_OK);
    check(status(&a) == S_OK && IXGameSaveImpl3_XGameSaveGetRemainingQuotaResult(saves,&a,&quota) == S_OK && quota == 256 * 1024 * 1024);
    IXGameSaveImpl3_XGameSaveCloseContainer(saves,c2);
    IXGameSaveImpl3_XGameSaveCloseProvider(saves,q);
    check(IXGameSaveImpl3_XGameSaveGetRemainingQuota(saves,q,&quota) == E_INVALIDARG);
    check(IXGameSaveImpl3_XGameSaveCreateContainer(saves,q,"slot/3",&c) == E_INVALIDARG);
}

static void package_updates(XUserHandle u, int expected_count, HRESULT expected)
{
    XStoreContextHandle c = NULL;
    XStorePackageUpdate updates[2];
    XAsyncBlock a = {0};
    UINT32 count = 99;
    check(IXStoreImpl6_XStoreCreateContext(store,u,&c) == S_OK);
    check(IXStoreImpl6_XStoreQueryGameAndDlcPackageUpdatesAsync(store,(void *)1,&a) == E_INVALIDARG);
    check(IXStoreImpl6_XStoreQueryGameAndDlcPackageUpdatesAsync(store,c,&a) == S_OK);
    check(status(&a) == expected);
    check(IXStoreImpl6_XStoreQueryGameAndDlcPackageUpdatesResultCount(store,&a,&count) == expected);
    if (FAILED(expected)) { check(!count); IXStoreImpl6_XStoreCloseContextHandle(store,c); return; }
    check(count == (UINT32)expected_count);
    if (expected_count)
    {
        memset(updates, 0, sizeof(updates));
        check(IXStoreImpl6_XStoreQueryGameAndDlcPackageUpdatesResult(store,&a,2,updates) == E_INVALIDARG);
        check(IXStoreImpl6_XStoreQueryGameAndDlcPackageUpdatesResult(store,&a,1,updates) == S_OK);
        check(!strcmp(updates[0].packageIdentifier,"2ef370abf18a4154880850e386d68eba") && !updates[0].isMandatory);
    }
    else check(IXStoreImpl6_XStoreQueryGameAndDlcPackageUpdatesResult(store,&a,0,NULL) == S_OK);
    IXStoreImpl6_XStoreCloseContextHandle(store,c);
}

int main(int argc, char **argv)
{
    HMODULE module;
    HRESULT (WINAPI *init)(ULONG,ULONG);
    HRESULT (WINAPI *query)(REFCLSID,REFIID,void **);
    XUserHandle u;
    if (argc != 3 || !(module = LoadLibraryA(argv[1]))) return 2;
    init = (void *)GetProcAddress(module,"InitializeApiImpl");
    query = (void *)GetProcAddress(module,"QueryApiImpl");
    if (!init || !query || FAILED(init(10001,3193))) return 2;
    if (FAILED(query(&CLSID_XThreadingImpl,&IID_IXThreadingImpl,(void **)&api)) ||
        FAILED(query(&CLSID_XUserImpl,&IID_IXUserImpl6,(void **)&user)) ||
        FAILED(IXUserImpl6_QueryInterface(user,&IID_IXUserGamertagImpl,(void **)&gamertags)) ||
        FAILED(query(&CLSID_XNetworkingImpl,&IID_IXNetworkingImpl2,(void **)&net)) ||
        FAILED(query(&CLSID_XStoreImpl,&IID_IXStoreImpl6,(void **)&store)) ||
        FAILED(query(&CLSID_XGameSaveImpl,&IID_IXGameSaveImpl3,(void **)&saves))) return 2;
    u = add_user();
    if (!strcmp(argv[2],"success")) { profile(u,S_OK); tokens(u,S_OK); security(S_OK); store_context(u); store_queries(u,S_OK); store_product_fields(u); store_license_token(u,S_OK); gamesave(u); package_updates(u,0,S_OK); }
    else if (!strcmp(argv[2],"package-update")) { profile(u,S_OK); tokens(u,S_OK); security(S_OK); package_updates(u,1,S_OK); }
    else if (!strcmp(argv[2],"package-update-fail")) { profile(u,S_OK); tokens(u,S_OK); security(S_OK); package_updates(u,0,E_GAMEUSER_FAILED_TO_GET_TOKEN); }
    else if (!strcmp(argv[2],"license-token-fail")) { profile(u,S_OK); tokens(u,S_OK); security(S_OK); store_license_token(u,E_GAMEUSER_FAILED_TO_GET_TOKEN); }
    else if (!strcmp(argv[2],"store-fail")) { profile(u,S_OK); tokens(u,S_OK); security(S_OK); store_queries(u,E_GAMEUSER_FAILED_TO_GET_TOKEN); }
    else if (!strcmp(argv[2],"security-fail")) { profile(u,S_OK); tokens(u,S_OK); security(E_GAMEUSER_FAILED_TO_GET_TOKEN); }
    else if (!strcmp(argv[2],"profile-fail")) { profile(u,E_GAMEUSER_FAILED_TO_GET_TOKEN); tokens(u,S_OK); }
    else if (!strcmp(argv[2],"token-fail")) { profile(u,S_OK); tokens(u,E_GAMEUSER_FAILED_TO_GET_TOKEN); }
    else return 2;
    printf("%ld checks, %ld failures\n",checks,failures);
    return failures ? 1 : 0;
}
