/*********************************************************
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation version 2 and no later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 *********************************************************/

/*
 * vmciQueuePair.c --
 *
 *     Implements the VMCI QueuePair API.
 */

#ifdef __linux__
#  include "driver-config.h"
#  include <asm/page.h>
#  include <linux/module.h>
#elif defined(_WIN32)
#  include <wdm.h>
#elif defined(__APPLE__)
#  include <IOKit/IOLib.h>
#endif /* __linux__ */

#include "vm_assert.h"
#include "vm_atomic.h"
#include "vmci_handle_array.h"
#include "vmci_kernel_if.h"
#include "vmciEvent.h"
#include "vmciInt.h"
#include "vmciKernelAPI.h"
#include "vmciQueuePairInt.h"
#include "vmciUtil.h"
#include "circList.h"

#define LGPFX "VMCIQueuePair: "

typedef struct QueuePairEntry {
   VMCIHandle handle;
   VMCIId     peer;
   uint32     flags;
   uint64     produceSize;
   uint64     consumeSize;
   uint64     numPPNs;
   PPNSet     ppnSet;
   void      *produceQ;
   void      *consumeQ;
   uint32     refCount;
   Bool       hibernateFailure;
   ListItem   listItem;
} QueuePairEntry;

typedef struct QueuePairList {
   ListItem      *head;
   Atomic_uint32  hibernate;
   VMCIMutex      mutex;
} QueuePairList;

static QueuePairList queuePairList;
static VMCIHandleArray *hibernateFailedList;
static VMCILock hibernateFailedListLock;

static QueuePairEntry *QueuePairList_FindEntry(VMCIHandle handle);
static void QueuePairList_AddEntry(QueuePairEntry *entry);
static void QueuePairList_RemoveEntry(QueuePairEntry *entry);
static QueuePairEntry *QueuePairList_GetHead(void);
static QueuePairEntry *QueuePairEntryCreate(VMCIHandle handle,
                                            VMCIId peer, uint32 flags,
                                            uint64 produceSize,
                                            uint64 consumeSize,
                                            void *produceQ, void *consumeQ);
static void QueuePairEntryDestroy(QueuePairEntry *entry);
static int VMCIQueuePairAlloc_HyperCall(const QueuePairEntry *entry);
static int VMCIQueuePairAllocHelper(VMCIHandle *handle, VMCIQueue **produceQ,
                                    uint64 produceSize, VMCIQueue **consumeQ,
                                    uint64 consumeSize,
                                    VMCIId peer, uint32 flags);
static int VMCIQueuePairDetachHelper(VMCIHandle handle);
static int VMCIQueuePairDetachHyperCall(VMCIHandle handle);
static int QueuePairNotifyPeerLocal(Bool attach, VMCIHandle handle);
static void VMCIQPMarkHibernateFailed(QueuePairEntry *entry);
static void VMCIQPUnmarkHibernateFailed(QueuePairEntry *entry);


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairLock_Init --
 *
 *      Creates the lock protecting the QueuePair list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
QueuePairLock_Init(void)
{
   VMCIMutex_Init(&queuePairList.mutex);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairLock_Destroy --
 *
 *      Destroys the lock protecting the QueuePair list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
QueuePairLock_Destroy(void)
{
   VMCIMutex_Destroy(&queuePairList.mutex); /* No-op on Linux and Windows. */
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_Lock --
 *
 *      Acquires the lock protecting the QueuePair list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
QueuePairList_Lock(void)
{
   VMCIMutex_Acquire(&queuePairList.mutex);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_Unlock --
 *
 *      Releases the lock protecting the QueuePair list.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static INLINE void
QueuePairList_Unlock(void)
{
   VMCIMutex_Release(&queuePairList.mutex);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePair_Init --
 *
 *      Initalizes QueuePair data structure state.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCIQueuePair_Init(void)
{
   queuePairList.head = NULL;
   Atomic_Write(&queuePairList.hibernate, 0);
   QueuePairLock_Init();
   hibernateFailedList = VMCIHandleArray_Create(0);

   /*
    * The lock rank must be lower than subscriberLock in vmciEvent,
    * since we hold the hibernateFailedListLock while generating
    * detach events.
    */

   VMCI_InitLock(&hibernateFailedListLock,
                 "VMCIQPHibernateFailed",
                 VMCI_LOCK_RANK_MIDDLE_BH);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePair_Exit --
 *
 *      Destroys all QueuePairs. Makes hypercalls to detach from QueuePairs.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCIQueuePair_Exit(void)
{
   QueuePairEntry *entry;

   QueuePairList_Lock();

   while ((entry = QueuePairList_GetHead())) {
      /*
       * Don't make a hypercall for local QueuePairs.
       */
      if (!(entry->flags & VMCI_QPFLAG_LOCAL)) {
         VMCIQueuePairDetachMsg detachMsg;

         detachMsg.hdr.dst = VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
                                              VMCI_QUEUEPAIR_DETACH);
         detachMsg.hdr.src = VMCI_ANON_SRC_HANDLE;
         detachMsg.hdr.payloadSize = sizeof entry->handle;
         detachMsg.handle = entry->handle;

         (void)VMCI_SendDatagram((VMCIDatagram *)&detachMsg);
      }
      /*
       * We cannot fail the exit, so let's reset refCount.
       */
      entry->refCount = 0;
      QueuePairList_RemoveEntry(entry);
      QueuePairEntryDestroy(entry);
   }

   Atomic_Write(&queuePairList.hibernate, 0);
   QueuePairList_Unlock();
   QueuePairLock_Destroy();
   VMCI_CleanupLock(&hibernateFailedListLock);
   VMCIHandleArray_Destroy(hibernateFailedList);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePair_Sync --
 *
 *      Use this as a synchronization point when setting globals, for example,
 *      during device shutdown.
 *
 * Results:
 *      TRUE.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
VMCIQueuePair_Sync(void)
{
   QueuePairList_Lock();
   QueuePairList_Unlock();
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_FindEntry --
 *
 *    Searches the list of QueuePairs to find if an entry already exists.
 *    Assumes that the lock on the list is held.
 *
 * Results:
 *    Pointer to the entry if it exists, NULL otherwise.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static QueuePairEntry *
QueuePairList_FindEntry(VMCIHandle handle) // IN:
{
   ListItem *next;

   if (VMCI_HANDLE_INVALID(handle)) {
      return NULL;
   }

   LIST_SCAN(next, queuePairList.head) {
      QueuePairEntry *entry = LIST_CONTAINER(next, QueuePairEntry, listItem);

      if (VMCI_HANDLE_EQUAL(entry->handle, handle)) {
         return entry;
      }
   }

   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_AddEntry --
 *
 *    Appends a QueuePair entry to the list. Assumes that the lock on the
 *    list is held.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static void
QueuePairList_AddEntry(QueuePairEntry *entry) // IN:
{
   if (entry) {
      LIST_QUEUE(&entry->listItem, &queuePairList.head);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_RemoveEntry --
 *
 *    Removes a QueuePair entry from the list. Assumes that the lock on the
 *    list is held.
 *
 * Results:
 *    None.
 *
 * Side effects:
 *    None.
 *
 *-----------------------------------------------------------------------------
 */

static void
QueuePairList_RemoveEntry(QueuePairEntry *entry) // IN:
{
   if (entry) {
      LIST_DEL(&entry->listItem, &queuePairList.head);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairList_GetHead --
 *
 *      Returns the entry from the head of the list. Assumes that the list is
 *      locked.
 *
 * Results:
 *      Pointer to entry.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static QueuePairEntry *
QueuePairList_GetHead(void)
{
   ListItem *first = LIST_FIRST(queuePairList.head);

   if (first) {
      QueuePairEntry *entry = LIST_CONTAINER(first, QueuePairEntry, listItem);
      return entry;
   }

   return NULL;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePair_Alloc --
 *
 *      Allocates a VMCI QueuePair. Only checks validity of input arguments.
 *      Real work is done in the OS-specific helper routine.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIQueuePair_Alloc(VMCIHandle *handle,     // IN/OUT:
                    VMCIQueue  **produceQ,  // OUT:
                    uint64     produceSize, // IN:
                    VMCIQueue  **consumeQ,  // OUT:
                    uint64     consumeSize, // IN:
                    VMCIId     peer,        // IN:
                    uint32     flags)       // IN:
{
   ASSERT_ON_COMPILE(sizeof(VMCIQueueHeader) <= PAGE_SIZE);

   return VMCIQueuePair_AllocPriv(handle, produceQ, produceSize, consumeQ, consumeSize, peer, flags, VMCI_NO_PRIVILEGE_FLAGS);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePair_AllocPriv --
 *
 *      Provided for compatibility with the host API. Always returns an error
 *      since requesting privileges from the guest is not allowed. Use
 *      VMCIQueuePair_Alloc instead.
 *
 * Results:
 *      An error.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIQueuePair_AllocPriv(VMCIHandle *handle,           // IN/OUT:
                        VMCIQueue  **produceQ,        // OUT:
                        uint64     produceSize,       // IN:
                        VMCIQueue  **consumeQ,        // OUT:
                        uint64     consumeSize,       // IN:
                        VMCIId     peer,              // IN:
                        uint32     flags,             // IN:
                        VMCIPrivilegeFlags privFlags) // IN:
{
   if (privFlags != VMCI_NO_PRIVILEGE_FLAGS) {
      return VMCI_ERROR_NO_ACCESS;
   }

   if (!handle || !produceQ || !consumeQ || (!produceSize && !consumeSize) ||
       (flags & ~VMCI_QP_ALL_FLAGS)) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   return VMCIQueuePairAllocHelper(handle, produceQ, produceSize, consumeQ,
                                   consumeSize, peer, flags);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePair_Detach --
 *
 *      Detaches from a VMCI QueuePair. Only checks validity of input argument.
 *      Real work is done in the OS-specific helper routine.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory is freed.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIQueuePair_Detach(VMCIHandle handle) // IN:
{
   if (VMCI_HANDLE_INVALID(handle)) {
      return VMCI_ERROR_INVALID_ARGS;
   }
   return VMCIQueuePairDetachHelper(handle);
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairEntryCreate --
 *
 *      Allocates and initializes a QueuePairEntry structure.  Allocates a
 *      QueuePair rid (and handle) iff the given entry has an invalid handle.
 *      0 through VMCI_RESERVED_RESOURCE_ID_MAX are reserved handles.  Assumes
 *      that the QP list lock is held by the caller.
 *
 * Results:
 *      Pointer to structure intialized.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

QueuePairEntry *
QueuePairEntryCreate(VMCIHandle handle,  // IN:
                     VMCIId peer,        // IN:
                     uint32 flags,       // IN:
                     uint64 produceSize, // IN:
                     uint64 consumeSize, // IN:
                     void *produceQ,     // IN:
                     void *consumeQ)     // IN:
{
   static VMCIId queuePairRID = VMCI_RESERVED_RESOURCE_ID_MAX + 1;
   QueuePairEntry *entry;
   const uint64 numPPNs = CEILING(produceSize, PAGE_SIZE) +
                          CEILING(consumeSize, PAGE_SIZE) +
                          2; /* One page each for the queue headers. */

   ASSERT((produceSize || consumeSize) && produceQ && consumeQ);

   if (VMCI_HANDLE_INVALID(handle)) {
      VMCIId contextID = VMCI_GetContextID();
      VMCIId oldRID = queuePairRID;

      /*
       * Generate a unique QueuePair rid.  Keep on trying until we wrap around
       * in the RID space.
       */
      ASSERT(oldRID > VMCI_RESERVED_RESOURCE_ID_MAX);
      do {
         handle = VMCI_MAKE_HANDLE(contextID, queuePairRID);
         entry = QueuePairList_FindEntry(handle);
         queuePairRID++;
         if (UNLIKELY(!queuePairRID)) {
            /*
             * Skip the reserved rids.
             */
            queuePairRID = VMCI_RESERVED_RESOURCE_ID_MAX + 1;
         }
      } while (entry && queuePairRID != oldRID);

      if (UNLIKELY(entry != NULL)) {
         ASSERT(queuePairRID == oldRID);
         /*
          * We wrapped around --- no rids were free.
          */
         return NULL;
      }
   }

   ASSERT(!VMCI_HANDLE_INVALID(handle) &&
          QueuePairList_FindEntry(handle) == NULL);
   entry = VMCI_AllocKernelMem(sizeof *entry, VMCI_MEMORY_NORMAL);
   if (entry) {
      entry->handle = handle;
      entry->peer = peer;
      entry->flags = flags;
      entry->produceSize = produceSize;
      entry->consumeSize = consumeSize;
      entry->numPPNs = numPPNs;
      memset(&entry->ppnSet, 0, sizeof entry->ppnSet);
      entry->produceQ = produceQ;
      entry->consumeQ = consumeQ;
      entry->refCount = 0;
      INIT_LIST_ITEM(&entry->listItem);
   }
   return entry;
}


/*
 *-----------------------------------------------------------------------------
 *
 * QueuePairEntryDestroy --
 *
 *      Frees a QueuePairEntry structure.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

void
QueuePairEntryDestroy(QueuePairEntry *entry) // IN:
{
   ASSERT(entry);
   ASSERT(entry->refCount == 0);

   VMCI_FreePPNSet(&entry->ppnSet);
   VMCI_FreeQueue(entry->produceQ, entry->produceSize);
   VMCI_FreeQueue(entry->consumeQ, entry->consumeSize);
   VMCI_FreeKernelMem(entry, sizeof *entry);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePairAlloc_HyperCall --
 *
 *      Helper to make a QueuePairAlloc hypercall.
 *
 * Results:
 *      Result of the hypercall.
 *
 * Side effects:
 *      Memory is allocated & freed.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIQueuePairAlloc_HyperCall(const QueuePairEntry *entry) // IN:
{
   VMCIQueuePairAllocMsg *allocMsg;
   size_t msgSize;
   int result;

   if (!entry || entry->numPPNs <= 2) {
      return VMCI_ERROR_INVALID_ARGS;
   }

   ASSERT(!(entry->flags & VMCI_QPFLAG_LOCAL));

   msgSize = sizeof *allocMsg + (size_t)entry->numPPNs * sizeof(PPN);
   allocMsg = VMCI_AllocKernelMem(msgSize, VMCI_MEMORY_NONPAGED);
   if (!allocMsg) {
      return VMCI_ERROR_NO_MEM;
   }

   allocMsg->hdr.dst = VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
					VMCI_QUEUEPAIR_ALLOC);
   allocMsg->hdr.src = VMCI_ANON_SRC_HANDLE;
   allocMsg->hdr.payloadSize = msgSize - VMCI_DG_HEADERSIZE;
   allocMsg->handle = entry->handle;
   allocMsg->peer = entry->peer;
   allocMsg->flags = entry->flags;
   allocMsg->produceSize = entry->produceSize;
   allocMsg->consumeSize = entry->consumeSize;
   allocMsg->numPPNs = entry->numPPNs;
   result = VMCI_PopulatePPNList((uint8 *)allocMsg + sizeof *allocMsg, &entry->ppnSet);
   if (result == VMCI_SUCCESS) {
      result = VMCI_SendDatagram((VMCIDatagram *)allocMsg);
   }
   VMCI_FreeKernelMem(allocMsg, msgSize);

   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePairAllocHelper --
 *
 *      Helper for VMCI QueuePairAlloc. Allocates physical pages for the
 *      QueuePair. Makes OS dependent calls through generic wrappers.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory is allocated.
 *
 *-----------------------------------------------------------------------------
 */

static int
VMCIQueuePairAllocHelper(VMCIHandle *handle,   // IN/OUT:
                         VMCIQueue **produceQ, // OUT:
                         uint64 produceSize,   // IN:
                         VMCIQueue **consumeQ, // OUT:
                         uint64 consumeSize,   // IN:
                         VMCIId peer,          // IN:
                         uint32 flags)         // IN:
{
   const uint64 numProducePages = CEILING(produceSize, PAGE_SIZE) + 1;
   const uint64 numConsumePages = CEILING(consumeSize, PAGE_SIZE) + 1;
   void *myProduceQ = NULL;
   void *myConsumeQ = NULL;
   int result;
   QueuePairEntry *queuePairEntry = NULL;

   /*
    * XXX Check for possible overflow of 'size' arguments when passed to
    * compat_get_order (after some arithmetic ops).
    */

   ASSERT(handle && produceQ && consumeQ && (produceSize || consumeSize));

   QueuePairList_Lock();

   /* Do not allow alloc/attach if the device is being shutdown. */
   if (VMCI_DeviceShutdown()) {
      result = VMCI_ERROR_DEVICE_NOT_FOUND;
      goto error;
   }

   if ((Atomic_Read(&queuePairList.hibernate) == 1) &&
       !(flags & VMCI_QPFLAG_LOCAL)) {
      /*
       * While guest OS is in hibernate state, creating non-local
       * queue pairs is not allowed after the point where the VMCI
       * guest driver converted the existing queue pairs to local
       * ones.
       */

      result = VMCI_ERROR_UNAVAILABLE;
      goto error;
   }

   if ((queuePairEntry = QueuePairList_FindEntry(*handle))) {
      if (queuePairEntry->flags & VMCI_QPFLAG_LOCAL) {
         /* Local attach case. */
         if (queuePairEntry->refCount > 1) {
            VMCI_DEBUG_LOG(4, (LGPFX"Error attempting to attach more than "
                               "once.\n"));
            result = VMCI_ERROR_UNAVAILABLE;
            goto errorKeepEntry;
         }

         if (queuePairEntry->produceSize != consumeSize ||
             queuePairEntry->consumeSize != produceSize ||
             queuePairEntry->flags != (flags & ~VMCI_QPFLAG_ATTACH_ONLY)) {
            VMCI_DEBUG_LOG(4, (LGPFX"Error mismatched queue pair in local "
                               "attach.\n"));
            result = VMCI_ERROR_QUEUEPAIR_MISMATCH;
            goto errorKeepEntry;
         }

         /*
          * Do a local attach.  We swap the consume and produce queues for the
          * attacher and deliver an attach event.
          */
         result = QueuePairNotifyPeerLocal(TRUE, *handle);
         if (result < VMCI_SUCCESS) {
            goto errorKeepEntry;
         }
         myProduceQ = queuePairEntry->consumeQ;
         myConsumeQ = queuePairEntry->produceQ;
         goto out;
      }
      result = VMCI_ERROR_ALREADY_EXISTS;
      goto errorKeepEntry;
   }

   myProduceQ = VMCI_AllocQueue(produceSize);
   if (!myProduceQ) {
      VMCI_WARNING((LGPFX"Error allocating pages for produce queue.\n"));
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   myConsumeQ = VMCI_AllocQueue(consumeSize);
   if (!myConsumeQ) {
      VMCI_WARNING((LGPFX"Error allocating pages for consume queue.\n"));
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   queuePairEntry = QueuePairEntryCreate(*handle, peer, flags,
                                         produceSize, consumeSize,
                                         myProduceQ, myConsumeQ);
   if (!queuePairEntry) {
      VMCI_WARNING((LGPFX"Error allocating memory in %s.\n", __FUNCTION__));
      result = VMCI_ERROR_NO_MEM;
      goto error;
   }

   result = VMCI_AllocPPNSet(myProduceQ, numProducePages, myConsumeQ,
                             numConsumePages, &queuePairEntry->ppnSet);
   if (result < VMCI_SUCCESS) {
      VMCI_WARNING((LGPFX"VMCI_AllocPPNSet failed.\n"));
      goto error;
   }

   /*
    * It's only necessary to notify the host if this queue pair will be
    * attached to from another context.
    */
   if (queuePairEntry->flags & VMCI_QPFLAG_LOCAL) {
      /* Local create case. */
      VMCIId contextId = VMCI_GetContextID();

      /*
       * Enforce similar checks on local queue pairs as we do for regular ones.
       * The handle's context must match the creator or attacher context id
       * (here they are both the current context id) and the attach-only flag
       * cannot exist during create.  We also ensure specified peer is this
       * context or an invalid one.
       */
      if (queuePairEntry->handle.context != contextId ||
          (queuePairEntry->peer != VMCI_INVALID_ID &&
           queuePairEntry->peer != contextId)) {
         result = VMCI_ERROR_NO_ACCESS;
         goto error;
      }

      if (queuePairEntry->flags & VMCI_QPFLAG_ATTACH_ONLY) {
         result = VMCI_ERROR_NOT_FOUND;
         goto error;
      }
   } else {
      result = VMCIQueuePairAlloc_HyperCall(queuePairEntry);
      if (result < VMCI_SUCCESS) {
         VMCI_WARNING((LGPFX"VMCIQueuePairAlloc_HyperCall result = %d.\n",
                       result));
         goto error;
      }
   }

   VMCI_InitQueueMutex((VMCIQueue *)myProduceQ, (VMCIQueue *)myConsumeQ);

   QueuePairList_AddEntry(queuePairEntry);

out:
   queuePairEntry->refCount++;
   *handle = queuePairEntry->handle;
   *produceQ = (VMCIQueue *)myProduceQ;
   *consumeQ = (VMCIQueue *)myConsumeQ;

   /*
    * We should initialize the queue pair header pages on a local queue pair
    * create.  For non-local queue pairs, the hypervisor initializes the header
    * pages in the create step.
    */
   if ((queuePairEntry->flags & VMCI_QPFLAG_LOCAL) &&
       queuePairEntry->refCount == 1) {
      VMCIQueueHeader_Init((*produceQ)->qHeader, *handle);
      VMCIQueueHeader_Init((*consumeQ)->qHeader, *handle);
   }

   QueuePairList_Unlock();

   return VMCI_SUCCESS;

error:
   QueuePairList_Unlock();
   if (queuePairEntry) {
      /* The queues will be freed inside the destroy routine. */
      QueuePairEntryDestroy(queuePairEntry);
   } else {
      if (myProduceQ) {
         VMCI_FreeQueue(myProduceQ, produceSize);
      }
      if (myConsumeQ) {
         VMCI_FreeQueue(myConsumeQ, consumeSize);
      }
   }
   return result;

errorKeepEntry:
   /* This path should only be used when an existing entry was found. */
   ASSERT(queuePairEntry->refCount > 0);
   QueuePairList_Unlock();
   return result;
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePairDetachHyperCall --
 *
 *      Helper to make a QueuePairDetach hypercall.
 *
 * Results:
 *      Result of the hypercall.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

int
VMCIQueuePairDetachHyperCall(VMCIHandle handle) // IN:
{
   VMCIQueuePairDetachMsg detachMsg;

   detachMsg.hdr.dst = VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
                                        VMCI_QUEUEPAIR_DETACH);
   detachMsg.hdr.src = VMCI_ANON_SRC_HANDLE;
   detachMsg.hdr.payloadSize = sizeof handle;
   detachMsg.handle = handle;

   return VMCI_SendDatagram((VMCIDatagram *)&detachMsg);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQueuePairDetachHelper --
 *
 *      Helper for VMCI QueuePair detach interface on Linux. Frees the physical
 *      pages for the QueuePair.
 *
 * Results:
 *      Success or failure.
 *
 * Side effects:
 *      Memory may be freed.
 *
 *-----------------------------------------------------------------------------
 */

static int
VMCIQueuePairDetachHelper(VMCIHandle handle)   // IN:
{
   int result;
   QueuePairEntry *entry;
   uint32 refCount;

   ASSERT(!VMCI_HANDLE_INVALID(handle));

   QueuePairList_Lock();

   entry = QueuePairList_FindEntry(handle);
   if (!entry) {
      result = VMCI_ERROR_NOT_FOUND;
      goto out;
   }

   ASSERT(entry->refCount >= 1);

   if (entry->flags & VMCI_QPFLAG_LOCAL) {
      result = VMCI_SUCCESS;

      if (entry->refCount > 1) {
         result = QueuePairNotifyPeerLocal(FALSE, handle);
         if (result < VMCI_SUCCESS) {
            goto out;
         }
      }
   } else {
      result = VMCIQueuePairDetachHyperCall(handle);
      if (entry->hibernateFailure) {
         if (result == VMCI_ERROR_NOT_FOUND) {
            /*
             * If a queue pair detach failed when entering
             * hibernation, the guest driver and the device may
             * disagree on its existence when coming out of
             * hibernation. The guest driver will regard it as a
             * non-local queue pair, but the device state is gone,
             * since the device has been powered off. In this case, we
             * treat the queue pair as a local queue pair with no
             * peer.
             */

            ASSERT(entry->refCount == 1);
            result = VMCI_SUCCESS;
         }
         if (result == VMCI_SUCCESS) {
            VMCIQPUnmarkHibernateFailed(entry);
         }
      }
   }

out:
   if (result >= VMCI_SUCCESS) {
      entry->refCount--;

      if (entry->refCount == 0) {
         QueuePairList_RemoveEntry(entry);
      }
   }

   /* If we didn't remove the entry, this could change once we unlock. */
   refCount = entry ? entry->refCount :
                      0xffffffff; /*
                                   * Value does not matter, silence the
                                   * compiler.
                                   */

   QueuePairList_Unlock();

   if (result >= VMCI_SUCCESS && refCount == 0) {
      QueuePairEntryDestroy(entry);
   }
   return result;
}


/*
 *----------------------------------------------------------------------------
 *
 * QueuePairNotifyPeerLocal --
 *
 *      Dispatches a queue pair event message directly into the local event
 *      queue.
 *
 * Results:
 *      VMCI_SUCCESS on success, error code otherwise
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------------
 */

static int
QueuePairNotifyPeerLocal(Bool attach,           // IN: attach or detach?
                         VMCIHandle handle)     // IN: queue pair handle
{
   VMCIEventMsg *eMsg;
   VMCIEventPayload_QP *ePayload;
   /* buf is only 48 bytes. */
   char buf[sizeof *eMsg + sizeof *ePayload];
   VMCIId contextId;

   contextId = VMCI_GetContextID();

   eMsg = (VMCIEventMsg *)buf;
   ePayload = VMCIEventMsgPayload(eMsg);

   eMsg->hdr.dst = VMCI_MAKE_HANDLE(contextId, VMCI_EVENT_HANDLER);
   eMsg->hdr.src = VMCI_MAKE_HANDLE(VMCI_HYPERVISOR_CONTEXT_ID,
                                    VMCI_CONTEXT_RESOURCE_ID);
   eMsg->hdr.payloadSize = sizeof *eMsg + sizeof *ePayload - sizeof eMsg->hdr;
   eMsg->eventData.event = attach ? VMCI_EVENT_QP_PEER_ATTACH :
                                    VMCI_EVENT_QP_PEER_DETACH;
   ePayload->peerId = contextId;
   ePayload->handle = handle;

   return VMCIEvent_Dispatch((VMCIDatagram *)eMsg);
}


/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQPMarkHibernateFailed --
 *
 *      Helper function that marks a queue pair entry as not being
 *      converted to a local version during hibernation. Must be
 *      called with the queue pair list lock held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
VMCIQPMarkHibernateFailed(QueuePairEntry *entry) // IN
{
   VMCILockFlags flags;
   VMCIHandle handle;

   /*
    * entry->handle is located in paged memory, so it can't be
    * accessed while holding a spinlock.
    */

   handle = entry->handle;
   entry->hibernateFailure = TRUE;
   VMCI_GrabLock_BH(&hibernateFailedListLock, &flags);
   VMCIHandleArray_AppendEntry(&hibernateFailedList, handle);
   VMCI_ReleaseLock_BH(&hibernateFailedListLock, flags);
}

/*
 *-----------------------------------------------------------------------------
 *
 * VMCIQPUnmarkHibernateFailed --
 *
 *      Helper function that removes a queue pair entry from the group
 *      of handles marked as having failed hibernation. Must be called
 *      with the queue pair list lock held.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      None.
 *
 *-----------------------------------------------------------------------------
 */

static void
VMCIQPUnmarkHibernateFailed(QueuePairEntry *entry) // IN
{
   VMCILockFlags flags;
   VMCIHandle handle;

   /*
    * entry->handle is located in paged memory, so it can't be
    * accessed while holding a spinlock.
    */

   handle = entry->handle;
   entry->hibernateFailure = FALSE;
   VMCI_GrabLock_BH(&hibernateFailedListLock, &flags);
   VMCIHandleArray_RemoveEntry(hibernateFailedList, handle);
   VMCI_ReleaseLock_BH(&hibernateFailedListLock, flags);
}


/*
 *----------------------------------------------------------------------------
 *
 * VMCIQueuePair_Convert --
 *
 *      Queue pairs may be converted to local ones in two cases: when
 *      entering hibernation or when the device is powered off before
 *      entering a sleep mode. Below we first discuss the case of
 *      hibernation and then the case of entering sleep state.
 *
 *      When the guest enters hibernation, any non-local queue pairs
 *      will disconnect no later than at the time the VMCI device
 *      powers off. To preserve the content of the non-local queue
 *      pairs for this guest, we make a local copy of the content and
 *      disconnect from the queue pairs. This will ensure that the
 *      peer doesn't continue to update the queue pair state while the
 *      guest OS is checkpointing the memory (otherwise we might end
 *      up with a inconsistent snapshot where the pointers of the
 *      consume queue are checkpointed later than the data pages they
 *      point to, possibly indicating that non-valid data is
 *      valid). While we are in hibernation mode, we block the
 *      allocation of new non-local queue pairs. Note that while we
 *      are doing the conversion to local queue pairs, we are holding
 *      the queue pair list lock, which will prevent concurrent
 *      creation of additional non-local queue pairs.
 *
 *      The hibernation cannot fail, so if we are unable to either
 *      save the queue pair state or detach from a queue pair, we deal
 *      with it by keeping the queue pair around, and converting it to
 *      a local queue pair when going out of hibernation. Since
 *      failing a detach is highly unlikely (it would require a queue
 *      pair being actively used as part of a DMA operation), this is
 *      an acceptable fall back. Once we come back from hibernation,
 *      these queue pairs will no longer be external, so we simply
 *      mark them as local at that point.
 *
 *      For the sleep state, the VMCI device will also be put into the
 *      D3 power state, which may make the device inaccessible to the
 *      guest driver (Windows unmaps the I/O space). When entering
 *      sleep state, the hypervisor is likely to suspend the guest as
 *      well, which will again convert all queue pairs to local ones.
 *      However, VMCI device clients, e.g., VMCI Sockets, may attempt
 *      to use queue pairs after the device has been put into the D3
 *      power state, so we convert the queue pairs to local ones in
 *      that case as well. When exiting the sleep states, the device
 *      has not been reset, so all device state is still in sync with
 *      the device driver, so no further processing is necessary at
 *      that point.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      Queue pairs are detached.
 *
 *----------------------------------------------------------------------------
 */

void
VMCIQueuePair_Convert(Bool toLocal,     // IN
                      Bool deviceReset) // IN
{
   if (toLocal) {
      ListItem *next;

      QueuePairList_Lock();

      LIST_SCAN(next, queuePairList.head) {
         QueuePairEntry *entry = LIST_CONTAINER(next, QueuePairEntry, listItem);

         if (!(entry->flags & VMCI_QPFLAG_LOCAL)) {
            VMCIQueue *prodQ;
            VMCIQueue *consQ;
            void *oldProdQ;
            void *oldConsQ;
            int result;

            prodQ = (VMCIQueue *)entry->produceQ;
            consQ = (VMCIQueue *)entry->consumeQ;
            oldConsQ = oldProdQ = NULL;

            VMCI_AcquireQueueMutex(prodQ);

            result = VMCI_ConvertToLocalQueue(consQ, prodQ, entry->consumeSize,
                                              TRUE, &oldConsQ);
            if (result != VMCI_SUCCESS) {
               VMCI_WARNING((LGPFX"Hibernate failed to create local consume "
                             "queue from handle %x:%x (error: %d)\n",
                             entry->handle.context, entry->handle.resource,
                             result));
               VMCI_ReleaseQueueMutex(prodQ);
               VMCIQPMarkHibernateFailed(entry);
               continue;
            }
            result = VMCI_ConvertToLocalQueue(prodQ, consQ, entry->produceSize,
                                              FALSE, &oldProdQ);
            if (result != VMCI_SUCCESS) {
               VMCI_WARNING((LGPFX"Hibernate failed to create local produce "
                             "queue from handle %x:%x (error: %d)\n",
                             entry->handle.context, entry->handle.resource,
                             result));
               VMCI_RevertToNonLocalQueue(consQ, oldConsQ, entry->consumeSize);
               VMCI_ReleaseQueueMutex(prodQ);
               VMCIQPMarkHibernateFailed(entry);
               continue;
            }

            /*
             * Now that the contents of the queue pair has been saved,
             * we can detach from the non-local queue pair. This will
             * discard the content of the non-local queues.
             */

            result = VMCIQueuePairDetachHyperCall(entry->handle);
            if (result < VMCI_SUCCESS) {
               VMCI_WARNING((LGPFX"Hibernate failed to detach from handle "
                             "%x:%x\n",
                             entry->handle.context, entry->handle.resource));
               VMCI_RevertToNonLocalQueue(consQ, oldConsQ, entry->consumeSize);
               VMCI_RevertToNonLocalQueue(prodQ, oldProdQ, entry->produceSize);
               VMCI_ReleaseQueueMutex(prodQ);
               VMCIQPMarkHibernateFailed(entry);
               continue;
            }

            entry->flags |= VMCI_QPFLAG_LOCAL;

            VMCI_ReleaseQueueMutex(prodQ);

            VMCI_FreeQueueBuffer(oldProdQ, entry->produceSize);
            VMCI_FreeQueueBuffer(oldConsQ, entry->consumeSize);

            QueuePairNotifyPeerLocal(FALSE, entry->handle);
         }
      }
      Atomic_Write(&queuePairList.hibernate, 1);

      QueuePairList_Unlock();
   } else {
      VMCILockFlags flags;
      VMCIHandle handle;

      /*
       * When a guest enters hibernation, there may be queue pairs
       * around, that couldn't be converted to local queue
       * pairs. When coming out of hibernation, these queue pairs
       * will be restored as part of the guest main mem by the OS
       * hibernation code and they can now be regarded as local
       * versions. Since they are no longer connected, detach
       * notifications are sent to the local endpoint.
       */

      VMCI_GrabLock_BH(&hibernateFailedListLock, &flags);
      while (VMCIHandleArray_GetSize(hibernateFailedList) > 0) {
         handle = VMCIHandleArray_RemoveTail(hibernateFailedList);
         if (deviceReset) {
            QueuePairNotifyPeerLocal(FALSE, handle);
         }
      }
      VMCI_ReleaseLock_BH(&hibernateFailedListLock, flags);

      Atomic_Write(&queuePairList.hibernate, 0);
   }
}
