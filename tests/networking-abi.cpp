// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Public C++ structure/enum ABI check. LGPL-2.1-or-later. */
#include <windows.h>
#include <stddef.h>
#include "xnetworking.h"
static_assert(sizeof(XNetworkingConnectivityHint)==16, "connectivity size");
static_assert(alignof(XNetworkingConnectivityHint)==4, "connectivity alignment");
static_assert(offsetof(XNetworkingConnectivityHint,connectivityLevel)==0, "level offset");
static_assert(offsetof(XNetworkingConnectivityHint,connectivityCost)==4, "cost offset");
static_assert(offsetof(XNetworkingConnectivityHint,ianaInterfaceType)==8, "interface offset");
static_assert(offsetof(XNetworkingConnectivityHint,networkInitialized)==12, "initialized offset");
static_assert(offsetof(XNetworkingConnectivityHint,approachingDataLimit)==13, "limit offset");
static_assert(offsetof(XNetworkingConnectivityHint,overDataLimit)==14, "over limit offset");
static_assert(offsetof(XNetworkingConnectivityHint,roaming)==15, "roaming offset");
static_assert(sizeof(XTaskQueueRegistrationToken)==8, "registration token size");
static_assert(static_cast<UINT32>(XNetworkingConnectivityLevelHint::Unknown)==0, "unknown enum");
static_assert(static_cast<UINT32>(XNetworkingConnectivityLevelHint::ConstrainedInternetAccess)==4, "level enum");
static_assert(static_cast<UINT32>(XNetworkingConnectivityCostHint::Variable)==3, "cost enum");
