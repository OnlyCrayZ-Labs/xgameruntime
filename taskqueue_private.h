// MSFS 2024 compatibility modifications: 2026-10-22; see PUBLICATION-PROVENANCE.md.
/* Internal XTaskQueue implementation, LGPL-2.1-or-later. */
#ifndef XODUS_TASKQUEUE_PRIVATE_H
#define XODUS_TASKQUEUE_PRIVATE_H
HRESULT task_queue_create( XTaskQueueDispatchMode, XTaskQueueDispatchMode, XTaskQueueHandle * );
HRESULT task_queue_duplicate( XTaskQueueHandle, XTaskQueueHandle * );
void task_queue_close( XTaskQueueHandle );
HRESULT task_queue_submit( XTaskQueueHandle, XTaskQueuePort, void *, XTaskQueueCallback * );
HRESULT task_queue_submit_delayed( XTaskQueueHandle, XTaskQueuePort, UINT32, void *, XTaskQueueCallback * );
BOOLEAN task_queue_dispatch( XTaskQueueHandle, XTaskQueuePort, UINT32 );
HRESULT task_queue_terminate( XTaskQueueHandle, BOOLEAN, void *, XTaskQueueTerminatedCallback * );
BOOLEAN task_queue_remove_callback( XTaskQueueHandle, XTaskQueuePort, void *, XTaskQueueCallback * );
HRESULT task_queue_get_port( XTaskQueueHandle, XTaskQueuePort, XTaskQueuePortHandle * );
HRESULT task_queue_create_composite( XTaskQueuePortHandle, XTaskQueuePortHandle, XTaskQueueHandle * );
enum eventless_kind { EVENTLESS_USER_CHANGE, EVENTLESS_GAME_INVITE, EVENTLESS_GAME_PROTOCOL, EVENTLESS_GAME_LICENSE, EVENTLESS_UDP_PORT };
HRESULT eventless_register( enum eventless_kind, XTaskQueueHandle, void *, XTaskQueueRegistrationToken * );
BOOLEAN eventless_unregister( enum eventless_kind, XTaskQueueRegistrationToken );
void eventless_cleanup( void );
#endif
