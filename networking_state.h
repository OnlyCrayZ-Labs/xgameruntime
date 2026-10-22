// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Observable adapter state reduction. LGPL-2.1-or-later. */
#ifndef XODUS_NETWORKING_STATE_H
#define XODUS_NETWORKING_STATE_H
/* The caller supplies a successful IP Helper snapshot. A route/address alone
 * does not establish Internet access, cost, roaming or endpoint reachability. */
static void networking_reduce(const IP_ADAPTER_ADDRESSES *adapters, const MIB_IPFORWARD_TABLE2 *routes,
                              XNetworkingConnectivityHint *hint)
{
    const IP_ADAPTER_ADDRESSES *a, *best = NULL, *routed = NULL;
    ULONGLONG metric, best_metric = ~(ULONGLONG)0;
    ULONG i;
    memset(hint, 0, sizeof(*hint));
    hint->networkInitialized = TRUE;
    hint->connectivityLevel = XNetworkingConnectivityLevelHint_None;
    for (a = adapters; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
            !a->FirstUnicastAddress) continue;
        if (!best || (!best->FirstGatewayAddress && a->FirstGatewayAddress)) best = a;
        for (i = 0; routes && i < routes->NumEntries; ++i)
        {
            const MIB_IPFORWARD_ROW2 *row = &routes->Table[i];
            BOOL ipv6 = row->DestinationPrefix.Prefix.si_family == AF_INET6;
            if (row->DestinationPrefix.PrefixLength || !row->ValidLifetime ||
                row->InterfaceIndex != (ipv6 ? a->Ipv6IfIndex : a->IfIndex)) continue;
            metric = (ULONGLONG)row->Metric + (ipv6 ? a->Ipv6Metric : a->Ipv4Metric);
            if (metric < best_metric) { routed = a; best_metric = metric; }
        }
    }
    if (routed) best = routed;
    if (best)
    {
        hint->connectivityLevel = XNetworkingConnectivityLevelHint_LocalAccess;
        hint->ianaInterfaceType = best->IfType;
    }
}
#endif
