# MSFS 2024 source candidate

Upstream: https://github.com/xodus-gaming/xgameruntime
Upstream source revision: `791710510d9ba0746bbd60754215eb321800e4f0`.
License scope: LGPL-2.1-or-later and original per-file terms. Original copyright and license notices are retained.

Approved upstream ancestry is retained; private milestone Git commits are not.

Local compatibility changes are derived from the private experimental milestone.
They use the existing component licensing and retain original notices. Public
interface contracts are references, not redistributed proprietary SDK files.
No libHttpClient C++ threading implementation is imported by this candidate.
No game files, private runtime binaries, captured credentials or backups are included.

The recorded successful flight applies to the preserved private runtime. This
source candidate has not yet reproduced that runtime from a clean checkout.

Changes: scoped taskqueue/async in C; authenticated local Xodus IPC; local saves,
Store/title queries and observable networking; synthetic tests. TASKQUEUE.md and
NETWORKING.md record public API references and implementation limits.
