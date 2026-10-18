// friend:u (nn::friends) offline: PLA asks for the friend list at boot (nn::friends::GetFriendList) and crashed on
// the logging stub's missing IFriendService object. Answers like a console whose user has no friends and never goes
// online: empty lists, zero counts, events that are never signalled. Commands per switchbrew "Friend services".
#include <string.h>

#include "../hle.h"
#include "../ipc.h"

static uint32_t new_event_handle(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

static void friend_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:  // GetCompletionEvent
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    case 1:  // Cancel
        break;
    case 10100:  // GetFriendListIds(offset, uid, filter, pid) -> u32 count, ids
    case 10101:  // GetFriendList -> u32 count, Friend[]
    case 10400:  // GetBlockedUserListIds -> u32 count, ids
    case 20100:  // GetFriendCount -> u32
    case 20101:  // GetNewlyFriendCount -> u32
    case 20200:  // GetReceivedFriendRequestCount -> u32 count, u32 unread
    case 20201:  // GetFriendRequestList -> u32 count
    case 20300:  // GetFriendCandidateList -> u32 count
    case 20400:  // GetBlockedUserList -> u32 count
    case 20700:  // GetPlayHistoryList -> u32 count
        OUT(uint32_t, 0) = 0;
        OUT(uint32_t, 4) = 0;
        resp_size(resp, 8);
        break;
    case 10600:  // DeclareOpenOnlinePlaySession
    case 10601:  // DeclareCloseOnlinePlaySession
    case 10610:  // UpdateUserPresence
        break;
    case 10120:  // CheckFriendListAvailability -> bool
        OUT(uint8_t, 0) = 0;
        resp_size(resp, 4);
        break;
    default:
        LOG("[friends] IFriendService cmd %u (ok, empty)\n", req->cmd);
        resp_size(resp, 0x10);
    }
}

static void notification_service(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0:  // GetEvent
        resp->copy_handles[resp->ncopy++] = new_event_handle();
        break;
    case 1:  // Clear
        break;
    case 2:  // Pop: nothing queued (the event is never signalled, so this is not expected)
        resp->result = 0x2e << 9 | 121;  // friends NotificationQueueEmpty
        break;
    default:
        LOG("[friends] INotificationService cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("friend:u", friend_u) {
    (void)s;
    switch (req->cmd) {
    case 0:  // CreateFriendService
        ipc_return_object(resp, service_new("IFriendService", friend_service, NULL));
        break;
    case 1:  // CreateNotificationService(uid)
        ipc_return_object(resp, service_new("INotificationService", notification_service, NULL));
        break;
    case 2:  // CreateDaemonSuspendSessionService
        ipc_return_object(resp, service_new("IDaemonSuspendSessionService", notification_service, NULL));
        break;
    default:
        LOG("[friends] friend:u cmd %u (ok)\n", req->cmd);
    }
}
