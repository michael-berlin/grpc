/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "src/core/surface/call.h"
#include "src/core/channel/channel_stack.h"
#include "src/core/iomgr/alarm.h"
#include "src/core/support/string.h"
#include "src/core/surface/byte_buffer_queue.h"
#include "src/core/surface/channel.h"
#include "src/core/surface/completion_queue.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { REQ_INITIAL = 0, REQ_READY, REQ_DONE } req_state;

typedef enum {
  SEND_NOTHING,
  SEND_INITIAL_METADATA,
  SEND_BUFFERED_INITIAL_METADATA,
  SEND_MESSAGE,
  SEND_BUFFERED_MESSAGE,
  SEND_TRAILING_METADATA_AND_FINISH,
  SEND_FINISH
} send_action;

typedef struct {
  grpc_ioreq_completion_func on_complete;
  void *user_data;
  grpc_op_error status;
} completed_request;

/* See request_set in grpc_call below for a description */
#define REQSET_EMPTY 'X'
#define REQSET_DONE 'Y'

#define MAX_SEND_INITIAL_METADATA_COUNT 3

typedef struct {
  /* Overall status of the operation: starts OK, may degrade to
     non-OK */
  grpc_op_error status;
  /* Completion function to call at the end of the operation */
  grpc_ioreq_completion_func on_complete;
  void *user_data;
  /* a bit mask of which request ops are needed (1u << opid) */
  gpr_uint16 need_mask;
  /* a bit mask of which request ops are now completed */
  gpr_uint16 complete_mask;
} reqinfo_master;

/* Status data for a request can come from several sources; this
   enumerates them all, and acts as a priority sorting for which
   status to return to the application - earlier entries override
   later ones */
typedef enum {
  /* Status came from the application layer overriding whatever
     the wire says */
  STATUS_FROM_API_OVERRIDE = 0,
  /* Status was created by some internal channel stack operation */
  STATUS_FROM_CORE,
  /* Status came from 'the wire' - or somewhere below the surface
     layer */
  STATUS_FROM_WIRE,
  STATUS_SOURCE_COUNT
} status_source;

typedef struct {
  gpr_uint8 is_set;
  grpc_status_code code;
  grpc_mdstr *details;
} received_status;

/* How far through the GRPC stream have we read? */
typedef enum {
  /* We are still waiting for initial metadata to complete */
  READ_STATE_INITIAL = 0,
  /* We have gotten initial metadata, and are reading either
     messages or trailing metadata */
  READ_STATE_GOT_INITIAL_METADATA,
  /* The stream is closed for reading */
  READ_STATE_READ_CLOSED,
  /* The stream is closed for reading & writing */
  READ_STATE_STREAM_CLOSED
} read_state;

typedef enum {
  WRITE_STATE_INITIAL = 0,
  WRITE_STATE_STARTED,
  WRITE_STATE_WRITE_CLOSED
} write_state;

struct grpc_call {
  grpc_completion_queue *cq;
  grpc_channel *channel;
  grpc_mdctx *metadata_context;
  /* TODO(ctiller): share with cq if possible? */
  gpr_mu mu;

  /* how far through the stream have we read? */
  read_state read_state;
  /* how far through the stream have we written? */
  write_state write_state;
  /* client or server call */
  gpr_uint8 is_client;
  /* is the alarm set */
  gpr_uint8 have_alarm;
  /* are we currently performing a send operation */
  gpr_uint8 sending;
  /* are we currently performing a recv operation */
  gpr_uint8 receiving;
  /* are we currently completing requests */
  gpr_uint8 completing;
  /* pairs with completed_requests */
  gpr_uint8 num_completed_requests;
  /* are we currently reading a message? */
  gpr_uint8 reading_message;
  /* flags with bits corresponding to write states allowing us to determine
     what was sent */
  gpr_uint16 last_send_contains;

  /* Active ioreqs.
     request_set and request_data contain one element per active ioreq
     operation.

     request_set[op] is an integer specifying a set of operations to which
     the request belongs:
       - if it is < GRPC_IOREQ_OP_COUNT, then this operation is pending
         completion, and the integer represents to which group of operations
         the ioreq belongs. Each group is represented by one master, and the
         integer in request_set is an index into masters to find the master
         data.
       - if it is REQSET_EMPTY, the ioreq op is inactive and available to be
         started
       - finally, if request_set[op] is REQSET_DONE, then the operation is
         complete and unavailable to be started again

     request_data[op] is the request data as supplied by the initiator of
     a request, and is valid iff request_set[op] <= GRPC_IOREQ_OP_COUNT.
     The set fields are as per the request type specified by op.

     Finally, one element of masters is set per active _set_ of ioreq
     operations. It describes work left outstanding, result status, and
     what work to perform upon operation completion. As one ioreq of each
     op type can be active at once, by convention we choose the first element
     of the group to be the master -- ie the master of in-progress operation
     op is masters[request_set[op]]. This allows constant time allocation
     and a strong upper bound of a count of masters to be calculated. */
  gpr_uint8 request_set[GRPC_IOREQ_OP_COUNT];
  grpc_ioreq_data request_data[GRPC_IOREQ_OP_COUNT];
  reqinfo_master masters[GRPC_IOREQ_OP_COUNT];

  /* Dynamic array of ioreq's that have completed: the count of
     elements is queued in num_completed_requests.
     This list is built up under lock(), and flushed entirely during
     unlock().
     We know the upper bound of the number of elements as we can only
     have one ioreq of each type active at once. */
  completed_request completed_requests[GRPC_IOREQ_OP_COUNT];
  /* Incoming buffer of messages */
  grpc_byte_buffer_queue incoming_queue;
  /* Buffered read metadata waiting to be returned to the application.
     Element 0 is initial metadata, element 1 is trailing metadata. */
  grpc_metadata_array buffered_metadata[2];
  /* All metadata received - unreffed at once at the end of the call */
  grpc_mdelem **owned_metadata;
  size_t owned_metadata_count;
  size_t owned_metadata_capacity;

  /* Received call statuses from various sources */
  received_status status[STATUS_SOURCE_COUNT];

  /* Deadline alarm - if have_alarm is non-zero */
  grpc_alarm alarm;

  /* Call refcount - to keep the call alive during asynchronous operations */
  gpr_refcount internal_refcount;

  grpc_linked_mdelem send_initial_metadata[MAX_SEND_INITIAL_METADATA_COUNT];
  grpc_linked_mdelem status_link;
  grpc_linked_mdelem details_link;
  size_t send_initial_metadata_count;
  gpr_timespec send_deadline;

  grpc_stream_op_buffer send_ops;
  grpc_stream_op_buffer recv_ops;
  grpc_stream_state recv_state;

  gpr_slice_buffer incoming_message;
  gpr_uint32 incoming_message_length;
};

#define CALL_STACK_FROM_CALL(call) ((grpc_call_stack *)((call) + 1))
#define CALL_FROM_CALL_STACK(call_stack) (((grpc_call *)(call_stack)) - 1)
#define CALL_ELEM_FROM_CALL(call, idx) \
  grpc_call_stack_element(CALL_STACK_FROM_CALL(call), idx)
#define CALL_FROM_TOP_ELEM(top_elem) \
  CALL_FROM_CALL_STACK(grpc_call_stack_from_top_element(top_elem))

#define SWAP(type, x, y) \
  do {                   \
    type temp = x;       \
    x = y;               \
    y = temp;            \
  } while (0)

static void do_nothing(void *ignored, grpc_op_error also_ignored) {}
static void set_deadline_alarm(grpc_call *call, gpr_timespec deadline);
static void call_on_done_recv(void *call, int success);
static void call_on_done_send(void *call, int success);
static int fill_send_ops(grpc_call *call, grpc_transport_op *op);
static void execute_op(grpc_call *call, grpc_transport_op *op);
static void recv_metadata(grpc_call *call, grpc_metadata_batch *metadata);
static void finish_read_ops(grpc_call *call);

grpc_call *grpc_call_create(grpc_channel *channel, grpc_completion_queue *cq,
                            const void *server_transport_data,
                            grpc_mdelem **add_initial_metadata,
                            size_t add_initial_metadata_count,
                            gpr_timespec send_deadline) {
  size_t i;
  grpc_transport_op initial_op;
  grpc_transport_op *initial_op_ptr = NULL;
  grpc_channel_stack *channel_stack = grpc_channel_get_channel_stack(channel);
  grpc_call *call =
      gpr_malloc(sizeof(grpc_call) + channel_stack->call_stack_size);
  memset(call, 0, sizeof(grpc_call));
  gpr_mu_init(&call->mu);
  call->channel = channel;
  call->cq = cq;
  call->is_client = server_transport_data == NULL;
  for (i = 0; i < GRPC_IOREQ_OP_COUNT; i++) {
    call->request_set[i] = REQSET_EMPTY;
  }
  if (call->is_client) {
    call->request_set[GRPC_IOREQ_SEND_TRAILING_METADATA] = REQSET_DONE;
    call->request_set[GRPC_IOREQ_SEND_STATUS] = REQSET_DONE;
  }
  GPR_ASSERT(add_initial_metadata_count < MAX_SEND_INITIAL_METADATA_COUNT);
  for (i = 0; i < add_initial_metadata_count; i++) {
    call->send_initial_metadata[i].md = add_initial_metadata[i];
  }
  call->send_initial_metadata_count = add_initial_metadata_count;
  call->send_deadline = send_deadline;
  grpc_channel_internal_ref(channel);
  call->metadata_context = grpc_channel_get_metadata_context(channel);
  grpc_sopb_init(&call->send_ops);
  grpc_sopb_init(&call->recv_ops);
  gpr_slice_buffer_init(&call->incoming_message);
  /* dropped in destroy */
  gpr_ref_init(&call->internal_refcount, 1);
  /* server hack: start reads immediately so we can get initial metadata.
     TODO(ctiller): figure out a cleaner solution */
  if (!call->is_client) {
    memset(&initial_op, 0, sizeof(initial_op));
    initial_op.recv_ops = &call->recv_ops;
    initial_op.recv_state = &call->recv_state;
    initial_op.on_done_recv = call_on_done_recv;
    initial_op.recv_user_data = call;
    call->receiving = 1;
    GRPC_CALL_INTERNAL_REF(call, "receiving");
    initial_op_ptr = &initial_op;
  }
  grpc_call_stack_init(channel_stack, server_transport_data, initial_op_ptr,
                       CALL_STACK_FROM_CALL(call));
  if (gpr_time_cmp(send_deadline, gpr_inf_future) != 0) {
    set_deadline_alarm(call, send_deadline);
  }
  return call;
}

void grpc_call_set_completion_queue(grpc_call *call,
                                    grpc_completion_queue *cq) {
  call->cq = cq;
}

grpc_completion_queue *grpc_call_get_completion_queue(grpc_call *call) {
  return call->cq;
}

#ifdef GRPC_CALL_REF_COUNT_DEBUG
void grpc_call_internal_ref(grpc_call *c, const char *reason) {
  gpr_log(GPR_DEBUG, "CALL:   ref %p %d -> %d [%s]", c,
          c->internal_refcount.count, c->internal_refcount.count + 1, reason);
#else
void grpc_call_internal_ref(grpc_call *c) {
#endif
  gpr_ref(&c->internal_refcount);
}

static void destroy_call(void *call, int ignored_success) {
  size_t i;
  grpc_call *c = call;
  grpc_call_stack_destroy(CALL_STACK_FROM_CALL(c));
  grpc_channel_internal_unref(c->channel);
  gpr_mu_destroy(&c->mu);
  for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
    if (c->status[i].details) {
      grpc_mdstr_unref(c->status[i].details);
    }
  }
  for (i = 0; i < c->owned_metadata_count; i++) {
    grpc_mdelem_unref(c->owned_metadata[i]);
  }
  gpr_free(c->owned_metadata);
  for (i = 0; i < GPR_ARRAY_SIZE(c->buffered_metadata); i++) {
    gpr_free(c->buffered_metadata[i].metadata);
  }
  for (i = 0; i < c->send_initial_metadata_count; i++) {
    grpc_mdelem_unref(c->send_initial_metadata[i].md);
  }
  grpc_sopb_destroy(&c->send_ops);
  grpc_sopb_destroy(&c->recv_ops);
  grpc_bbq_destroy(&c->incoming_queue);
  gpr_slice_buffer_destroy(&c->incoming_message);
  gpr_free(c);
}

#ifdef GRPC_CALL_REF_COUNT_DEBUG
void grpc_call_internal_unref(grpc_call *c, const char *reason,
                              int allow_immediate_deletion) {
  gpr_log(GPR_DEBUG, "CALL: unref %p %d -> %d [%s]", c,
          c->internal_refcount.count, c->internal_refcount.count - 1, reason);
#else
void grpc_call_internal_unref(grpc_call *c, int allow_immediate_deletion) {
#endif
  if (gpr_unref(&c->internal_refcount)) {
    if (allow_immediate_deletion) {
      destroy_call(c, 1);
    } else {
      grpc_iomgr_add_callback(destroy_call, c);
    }
  }
}

static void set_status_code(grpc_call *call, status_source source,
                            gpr_uint32 status) {
  int flush;

  call->status[source].is_set = 1;
  call->status[source].code = status;

  if (call->is_client) {
    flush = status == GRPC_STATUS_CANCELLED;
  } else {
    flush = status != GRPC_STATUS_OK;
  }

  if (flush && !grpc_bbq_empty(&call->incoming_queue)) {
    grpc_bbq_flush(&call->incoming_queue);
  }
}

static void set_status_details(grpc_call *call, status_source source,
                               grpc_mdstr *status) {
  if (call->status[source].details != NULL) {
    grpc_mdstr_unref(call->status[source].details);
  }
  call->status[source].details = status;
}

static int is_op_live(grpc_call *call, grpc_ioreq_op op) {
  gpr_uint8 set = call->request_set[op];
  reqinfo_master *master;
  if (set >= GRPC_IOREQ_OP_COUNT) return 0;
  master = &call->masters[set];
  return (master->complete_mask & (1u << op)) == 0;
}

static void lock(grpc_call *call) { gpr_mu_lock(&call->mu); }

static int need_more_data(grpc_call *call) {
  return is_op_live(call, GRPC_IOREQ_RECV_INITIAL_METADATA) ||
         is_op_live(call, GRPC_IOREQ_RECV_MESSAGE) ||
         is_op_live(call, GRPC_IOREQ_RECV_TRAILING_METADATA) ||
         is_op_live(call, GRPC_IOREQ_RECV_STATUS) ||
         is_op_live(call, GRPC_IOREQ_RECV_STATUS_DETAILS) ||
         (is_op_live(call, GRPC_IOREQ_RECV_CLOSE) &&
          grpc_bbq_empty(&call->incoming_queue)) ||
         (call->write_state == WRITE_STATE_INITIAL && !call->is_client &&
          call->read_state != READ_STATE_STREAM_CLOSED);
}

static void unlock(grpc_call *call) {
  grpc_transport_op op;
  completed_request completed_requests[GRPC_IOREQ_OP_COUNT];
  int completing_requests = 0;
  int start_op = 0;
  int i;

  memset(&op, 0, sizeof(op));

  if (!call->receiving && need_more_data(call)) {
    op.recv_ops = &call->recv_ops;
    op.recv_state = &call->recv_state;
    op.on_done_recv = call_on_done_recv;
    op.recv_user_data = call;
    call->receiving = 1;
    GRPC_CALL_INTERNAL_REF(call, "receiving");
    start_op = 1;
  }

  if (!call->sending) {
    if (fill_send_ops(call, &op)) {
      call->sending = 1;
      GRPC_CALL_INTERNAL_REF(call, "sending");
      start_op = 1;
    }
  }

  if (!call->completing && call->num_completed_requests != 0) {
    completing_requests = call->num_completed_requests;
    memcpy(completed_requests, call->completed_requests,
           sizeof(completed_requests));
    call->num_completed_requests = 0;
    call->completing = 1;
    GRPC_CALL_INTERNAL_REF(call, "completing");
  }

  gpr_mu_unlock(&call->mu);

  if (start_op) {
    execute_op(call, &op);
  }

  if (completing_requests > 0) {
    for (i = 0; i < completing_requests; i++) {
      completed_requests[i].on_complete(call, completed_requests[i].status,
                                        completed_requests[i].user_data);
    }
    lock(call);
    call->completing = 0;
    unlock(call);
    GRPC_CALL_INTERNAL_UNREF(call, "completing", 0);
  }
}

static void get_final_status(grpc_call *call, grpc_ioreq_data out) {
  int i;
  for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
    if (call->status[i].is_set) {
      out.recv_status.set_value(call->status[i].code,
                                out.recv_status.user_data);
      return;
    }
  }
  if (call->is_client) {
    out.recv_status.set_value(GRPC_STATUS_UNKNOWN, out.recv_status.user_data);
  } else {
    out.recv_status.set_value(GRPC_STATUS_OK, out.recv_status.user_data);
  }
}

static void get_final_details(grpc_call *call, grpc_ioreq_data out) {
  int i;
  for (i = 0; i < STATUS_SOURCE_COUNT; i++) {
    if (call->status[i].is_set) {
      if (call->status[i].details) {
        gpr_slice details = call->status[i].details->slice;
        size_t len = GPR_SLICE_LENGTH(details);
        if (len + 1 > *out.recv_status_details.details_capacity) {
          *out.recv_status_details.details_capacity = GPR_MAX(
              len + 1, *out.recv_status_details.details_capacity * 3 / 2);
          *out.recv_status_details.details =
              gpr_realloc(*out.recv_status_details.details,
                          *out.recv_status_details.details_capacity);
        }
        memcpy(*out.recv_status_details.details, GPR_SLICE_START_PTR(details),
               len);
        (*out.recv_status_details.details)[len] = 0;
      } else {
        goto no_details;
      }
      return;
    }
  }

no_details:
  if (0 == *out.recv_status_details.details_capacity) {
    *out.recv_status_details.details_capacity = 8;
    *out.recv_status_details.details =
        gpr_malloc(*out.recv_status_details.details_capacity);
  }
  **out.recv_status_details.details = 0;
}

static void finish_live_ioreq_op(grpc_call *call, grpc_ioreq_op op,
                                 grpc_op_error status) {
  completed_request *cr;
  gpr_uint8 master_set = call->request_set[op];
  reqinfo_master *master;
  size_t i;
  /* ioreq is live: we need to do something */
  master = &call->masters[master_set];
  master->complete_mask |= 1u << op;
  if (status != GRPC_OP_OK) {
    master->status = status;
  }
  if (master->complete_mask == master->need_mask) {
    for (i = 0; i < GRPC_IOREQ_OP_COUNT; i++) {
      if (call->request_set[i] != master_set) {
        continue;
      }
      call->request_set[i] = REQSET_DONE;
      switch ((grpc_ioreq_op)i) {
        case GRPC_IOREQ_RECV_MESSAGE:
        case GRPC_IOREQ_SEND_MESSAGE:
          if (master->status == GRPC_OP_OK) {
            call->request_set[i] = REQSET_EMPTY;
          } else {
            call->write_state = WRITE_STATE_WRITE_CLOSED;
          }
          break;
        case GRPC_IOREQ_RECV_CLOSE:
        case GRPC_IOREQ_SEND_INITIAL_METADATA:
        case GRPC_IOREQ_SEND_TRAILING_METADATA:
        case GRPC_IOREQ_SEND_STATUS:
        case GRPC_IOREQ_SEND_CLOSE:
          break;
        case GRPC_IOREQ_RECV_STATUS:
          get_final_status(call, call->request_data[GRPC_IOREQ_RECV_STATUS]);
          break;
        case GRPC_IOREQ_RECV_STATUS_DETAILS:
          get_final_details(call,
                            call->request_data[GRPC_IOREQ_RECV_STATUS_DETAILS]);
          break;
        case GRPC_IOREQ_RECV_INITIAL_METADATA:
          SWAP(grpc_metadata_array, call->buffered_metadata[0],
               *call->request_data[GRPC_IOREQ_RECV_INITIAL_METADATA]
                    .recv_metadata);
          break;
        case GRPC_IOREQ_RECV_TRAILING_METADATA:
          SWAP(grpc_metadata_array, call->buffered_metadata[1],
               *call->request_data[GRPC_IOREQ_RECV_TRAILING_METADATA]
                    .recv_metadata);
          break;
        case GRPC_IOREQ_OP_COUNT:
          abort();
          break;
      }
    }
    cr = &call->completed_requests[call->num_completed_requests++];
    cr->status = master->status;
    cr->on_complete = master->on_complete;
    cr->user_data = master->user_data;
  }
}

static void finish_ioreq_op(grpc_call *call, grpc_ioreq_op op,
                            grpc_op_error status) {
  if (is_op_live(call, op)) {
    finish_live_ioreq_op(call, op, status);
  }
}

static void call_on_done_send(void *pc, int success) {
  grpc_call *call = pc;
  grpc_op_error error = success ? GRPC_OP_OK : GRPC_OP_ERROR;
  lock(call);
  if (call->last_send_contains & (1 << GRPC_IOREQ_SEND_INITIAL_METADATA)) {
    finish_ioreq_op(call, GRPC_IOREQ_SEND_INITIAL_METADATA, error);
  }
  if (call->last_send_contains & (1 << GRPC_IOREQ_SEND_MESSAGE)) {
    finish_ioreq_op(call, GRPC_IOREQ_SEND_MESSAGE, error);
  }
  if (call->last_send_contains & (1 << GRPC_IOREQ_SEND_CLOSE)) {
    finish_ioreq_op(call, GRPC_IOREQ_SEND_TRAILING_METADATA, error);
    finish_ioreq_op(call, GRPC_IOREQ_SEND_STATUS, error);
    finish_ioreq_op(call, GRPC_IOREQ_SEND_CLOSE, GRPC_OP_OK);
  }
  call->last_send_contains = 0;
  call->sending = 0;
  unlock(call);
  GRPC_CALL_INTERNAL_UNREF(call, "sending", 0);
}

static void finish_message(grpc_call *call) {
  /* TODO(ctiller): this could be a lot faster if coded directly */
  grpc_byte_buffer *byte_buffer = grpc_byte_buffer_create(
      call->incoming_message.slices, call->incoming_message.count);
  gpr_slice_buffer_reset_and_unref(&call->incoming_message);

  grpc_bbq_push(&call->incoming_queue, byte_buffer);

  GPR_ASSERT(call->incoming_message.count == 0);
  call->reading_message = 0;
}

static int begin_message(grpc_call *call, grpc_begin_message msg) {
  /* can't begin a message when we're still reading a message */
  if (call->reading_message) {
    char *message = NULL;
    gpr_asprintf(
        &message, "Message terminated early; read %d bytes, expected %d",
        (int)call->incoming_message.length, (int)call->incoming_message_length);
    grpc_call_cancel_with_status(call, GRPC_STATUS_INVALID_ARGUMENT, message);
    gpr_free(message);
    return 0;
  }
  /* stash away parameters, and prepare for incoming slices */
  if (msg.length > grpc_channel_get_max_message_length(call->channel)) {
    char *message = NULL;
    gpr_asprintf(
        &message,
        "Maximum message length of %d exceeded by a message of length %d",
        grpc_channel_get_max_message_length(call->channel), msg.length);
    grpc_call_cancel_with_status(call, GRPC_STATUS_INVALID_ARGUMENT, message);
    gpr_free(message);
    return 0;
  } else if (msg.length > 0) {
    call->reading_message = 1;
    call->incoming_message_length = msg.length;
    return 1;
  } else {
    finish_message(call);
    return 1;
  }
}

static int add_slice_to_message(grpc_call *call, gpr_slice slice) {
  if (GPR_SLICE_LENGTH(slice) == 0) {
    gpr_slice_unref(slice);
    return 1;
  }
  /* we have to be reading a message to know what to do here */
  if (!call->reading_message) {
    grpc_call_cancel_with_status(
        call, GRPC_STATUS_INVALID_ARGUMENT,
        "Received payload data while not reading a message");
    return 0;
  }
  /* append the slice to the incoming buffer */
  gpr_slice_buffer_add(&call->incoming_message, slice);
  if (call->incoming_message.length > call->incoming_message_length) {
    /* if we got too many bytes, complain */
    char *message = NULL;
    gpr_asprintf(
        &message, "Receiving message overflow; read %d bytes, expected %d",
        (int)call->incoming_message.length, (int)call->incoming_message_length);
    grpc_call_cancel_with_status(call, GRPC_STATUS_INVALID_ARGUMENT, message);
    gpr_free(message);
    return 0;
  } else if (call->incoming_message.length == call->incoming_message_length) {
    finish_message(call);
    return 1;
  } else {
    return 1;
  }
}

static void call_on_done_recv(void *pc, int success) {
  grpc_call *call = pc;
  size_t i;
  lock(call);
  call->receiving = 0;
  if (success) {
    for (i = 0; success && i < call->recv_ops.nops; i++) {
      grpc_stream_op *op = &call->recv_ops.ops[i];
      switch (op->type) {
        case GRPC_NO_OP:
          break;
        case GRPC_OP_METADATA:
          recv_metadata(call, &op->data.metadata);
          break;
        case GRPC_OP_BEGIN_MESSAGE:
          success = begin_message(call, op->data.begin_message);
          break;
        case GRPC_OP_SLICE:
          success = add_slice_to_message(call, op->data.slice);
          break;
      }
    }
    if (call->recv_state == GRPC_STREAM_RECV_CLOSED) {
      GPR_ASSERT(call->read_state <= READ_STATE_READ_CLOSED);
      call->read_state = READ_STATE_READ_CLOSED;
    }
    if (call->recv_state == GRPC_STREAM_CLOSED) {
      GPR_ASSERT(call->read_state <= READ_STATE_STREAM_CLOSED);
      call->read_state = READ_STATE_STREAM_CLOSED;
      if (call->have_alarm) {
        grpc_alarm_cancel(&call->alarm);
        call->have_alarm = 0;
      }
    }
    finish_read_ops(call);
  } else {
    finish_ioreq_op(call, GRPC_IOREQ_RECV_MESSAGE, GRPC_OP_ERROR);
    finish_ioreq_op(call, GRPC_IOREQ_RECV_STATUS, GRPC_OP_ERROR);
    finish_ioreq_op(call, GRPC_IOREQ_RECV_CLOSE, GRPC_OP_ERROR);
    finish_ioreq_op(call, GRPC_IOREQ_RECV_TRAILING_METADATA, GRPC_OP_ERROR);
    finish_ioreq_op(call, GRPC_IOREQ_RECV_INITIAL_METADATA, GRPC_OP_ERROR);
    finish_ioreq_op(call, GRPC_IOREQ_RECV_STATUS_DETAILS, GRPC_OP_ERROR);
  }
  call->recv_ops.nops = 0;
  unlock(call);

  GRPC_CALL_INTERNAL_UNREF(call, "receiving", 0);
}

static grpc_mdelem_list chain_metadata_from_app(grpc_call *call, size_t count,
                                                grpc_metadata *metadata) {
  size_t i;
  grpc_mdelem_list out;
  if (count == 0) {
    out.head = out.tail = NULL;
    return out;
  }
  for (i = 0; i < count; i++) {
    grpc_metadata *md = &metadata[i];
    grpc_metadata *next_md = (i == count - 1) ? NULL : &metadata[i + 1];
    grpc_metadata *prev_md = (i == 0) ? NULL : &metadata[i - 1];
    grpc_linked_mdelem *l = (grpc_linked_mdelem *)&md->internal_data;
    GPR_ASSERT(sizeof(grpc_linked_mdelem) == sizeof(md->internal_data));
    l->md = grpc_mdelem_from_string_and_buffer(call->metadata_context, md->key,
                                               (const gpr_uint8 *)md->value,
                                               md->value_length);
    l->next = next_md ? (grpc_linked_mdelem *)&next_md->internal_data : NULL;
    l->prev = prev_md ? (grpc_linked_mdelem *)&prev_md->internal_data : NULL;
  }
  out.head = (grpc_linked_mdelem *)&(metadata[0].internal_data);
  out.tail = (grpc_linked_mdelem *)&(metadata[count - 1].internal_data);
  return out;
}

/* Copy the contents of a byte buffer into stream ops */
static void copy_byte_buffer_to_stream_ops(grpc_byte_buffer *byte_buffer,
                                           grpc_stream_op_buffer *sopb) {
  size_t i;

  switch (byte_buffer->type) {
    case GRPC_BB_SLICE_BUFFER:
      for (i = 0; i < byte_buffer->data.slice_buffer.count; i++) {
        gpr_slice slice = byte_buffer->data.slice_buffer.slices[i];
        gpr_slice_ref(slice);
        grpc_sopb_add_slice(sopb, slice);
      }
      break;
  }
}

static int fill_send_ops(grpc_call *call, grpc_transport_op *op) {
  grpc_ioreq_data data;
  grpc_metadata_batch mdb;
  size_t i;
  char status_str[GPR_LTOA_MIN_BUFSIZE];
  GPR_ASSERT(op->send_ops == NULL);

  switch (call->write_state) {
    case WRITE_STATE_INITIAL:
      if (!is_op_live(call, GRPC_IOREQ_SEND_INITIAL_METADATA)) {
        break;
      }
      data = call->request_data[GRPC_IOREQ_SEND_INITIAL_METADATA];
      mdb.list = chain_metadata_from_app(call, data.send_metadata.count,
                                         data.send_metadata.metadata);
      mdb.garbage.head = mdb.garbage.tail = NULL;
      mdb.deadline = call->send_deadline;
      for (i = 0; i < call->send_initial_metadata_count; i++) {
        grpc_metadata_batch_link_head(&mdb, &call->send_initial_metadata[i]);
      }
      grpc_sopb_add_metadata(&call->send_ops, mdb);
      op->send_ops = &call->send_ops;
      op->bind_pollset = grpc_cq_pollset(call->cq);
      call->last_send_contains |= 1 << GRPC_IOREQ_SEND_INITIAL_METADATA;
      call->write_state = WRITE_STATE_STARTED;
      call->send_initial_metadata_count = 0;
    /* fall through intended */
    case WRITE_STATE_STARTED:
      if (is_op_live(call, GRPC_IOREQ_SEND_MESSAGE)) {
        data = call->request_data[GRPC_IOREQ_SEND_MESSAGE];
        grpc_sopb_add_begin_message(
            &call->send_ops, grpc_byte_buffer_length(data.send_message), 0);
        copy_byte_buffer_to_stream_ops(data.send_message, &call->send_ops);
        op->send_ops = &call->send_ops;
        call->last_send_contains |= 1 << GRPC_IOREQ_SEND_MESSAGE;
      }
      if (is_op_live(call, GRPC_IOREQ_SEND_CLOSE)) {
        op->is_last_send = 1;
        op->send_ops = &call->send_ops;
        call->last_send_contains |= 1 << GRPC_IOREQ_SEND_CLOSE;
        call->write_state = WRITE_STATE_WRITE_CLOSED;
        if (!call->is_client) {
          /* send trailing metadata */
          data = call->request_data[GRPC_IOREQ_SEND_TRAILING_METADATA];
          mdb.list = chain_metadata_from_app(call, data.send_metadata.count,
                                             data.send_metadata.metadata);
          mdb.garbage.head = mdb.garbage.tail = NULL;
          mdb.deadline = gpr_inf_future;
          /* send status */
          /* TODO(ctiller): cache common status values */
          data = call->request_data[GRPC_IOREQ_SEND_STATUS];
          gpr_ltoa(data.send_status.code, status_str);
          grpc_metadata_batch_add_tail(
              &mdb, &call->status_link,
              grpc_mdelem_from_metadata_strings(
                  call->metadata_context,
                  grpc_mdstr_ref(grpc_channel_get_status_string(call->channel)),
                  grpc_mdstr_from_string(call->metadata_context, status_str)));
          if (data.send_status.details) {
            grpc_metadata_batch_add_tail(
                &mdb, &call->details_link,
                grpc_mdelem_from_metadata_strings(
                    call->metadata_context,
                    grpc_mdstr_ref(
                        grpc_channel_get_message_string(call->channel)),
                    grpc_mdstr_from_string(call->metadata_context,
                                           data.send_status.details)));
          }
          grpc_sopb_add_metadata(&call->send_ops, mdb);
        }
      }
      break;
    case WRITE_STATE_WRITE_CLOSED:
      break;
  }
  if (op->send_ops) {
    op->on_done_send = call_on_done_send;
    op->send_user_data = call;
  }
  return op->send_ops != NULL;
}

static grpc_call_error start_ioreq_error(grpc_call *call,
                                         gpr_uint32 mutated_ops,
                                         grpc_call_error ret) {
  size_t i;
  for (i = 0; i < GRPC_IOREQ_OP_COUNT; i++) {
    if (mutated_ops & (1u << i)) {
      call->request_set[i] = REQSET_EMPTY;
    }
  }
  return ret;
}

static void finish_read_ops(grpc_call *call) {
  int empty;

  if (is_op_live(call, GRPC_IOREQ_RECV_MESSAGE)) {
    empty =
        (NULL == (*call->request_data[GRPC_IOREQ_RECV_MESSAGE].recv_message =
                      grpc_bbq_pop(&call->incoming_queue)));
    if (!empty) {
      finish_live_ioreq_op(call, GRPC_IOREQ_RECV_MESSAGE, GRPC_OP_OK);
      empty = grpc_bbq_empty(&call->incoming_queue);
    }
  } else {
    empty = grpc_bbq_empty(&call->incoming_queue);
  }

  switch (call->read_state) {
    case READ_STATE_STREAM_CLOSED:
      if (empty) {
        finish_ioreq_op(call, GRPC_IOREQ_RECV_CLOSE, GRPC_OP_OK);
      }
    /* fallthrough */
    case READ_STATE_READ_CLOSED:
      if (empty) {
        finish_ioreq_op(call, GRPC_IOREQ_RECV_MESSAGE, GRPC_OP_OK);
      }
      finish_ioreq_op(call, GRPC_IOREQ_RECV_STATUS, GRPC_OP_OK);
      finish_ioreq_op(call, GRPC_IOREQ_RECV_STATUS_DETAILS, GRPC_OP_OK);
      finish_ioreq_op(call, GRPC_IOREQ_RECV_TRAILING_METADATA, GRPC_OP_OK);
    /* fallthrough */
    case READ_STATE_GOT_INITIAL_METADATA:
      finish_ioreq_op(call, GRPC_IOREQ_RECV_INITIAL_METADATA, GRPC_OP_OK);
    /* fallthrough */
    case READ_STATE_INITIAL:
      /* do nothing */
      break;
  }
}

static void early_out_write_ops(grpc_call *call) {
  switch (call->write_state) {
    case WRITE_STATE_WRITE_CLOSED:
      finish_ioreq_op(call, GRPC_IOREQ_SEND_MESSAGE, GRPC_OP_ERROR);
      finish_ioreq_op(call, GRPC_IOREQ_SEND_STATUS, GRPC_OP_ERROR);
      finish_ioreq_op(call, GRPC_IOREQ_SEND_TRAILING_METADATA, GRPC_OP_ERROR);
      finish_ioreq_op(call, GRPC_IOREQ_SEND_CLOSE, GRPC_OP_OK);
    /* fallthrough */
    case WRITE_STATE_STARTED:
      finish_ioreq_op(call, GRPC_IOREQ_SEND_INITIAL_METADATA, GRPC_OP_ERROR);
    /* fallthrough */
    case WRITE_STATE_INITIAL:
      /* do nothing */
      break;
  }
}

static grpc_call_error start_ioreq(grpc_call *call, const grpc_ioreq *reqs,
                                   size_t nreqs,
                                   grpc_ioreq_completion_func completion,
                                   void *user_data) {
  size_t i;
  gpr_uint32 have_ops = 0;
  grpc_ioreq_op op;
  reqinfo_master *master;
  grpc_ioreq_data data;
  gpr_uint8 set;

  if (nreqs == 0) {
    return GRPC_CALL_OK;
  }

  set = reqs[0].op;

  for (i = 0; i < nreqs; i++) {
    op = reqs[i].op;
    if (call->request_set[op] < GRPC_IOREQ_OP_COUNT) {
      return start_ioreq_error(call, have_ops,
                               GRPC_CALL_ERROR_TOO_MANY_OPERATIONS);
    } else if (call->request_set[op] == REQSET_DONE) {
      return start_ioreq_error(call, have_ops, GRPC_CALL_ERROR_ALREADY_INVOKED);
    }
    have_ops |= 1u << op;
    data = reqs[i].data;

    call->request_data[op] = data;
    call->request_set[op] = set;
  }

  master = &call->masters[set];
  master->status = GRPC_OP_OK;
  master->need_mask = have_ops;
  master->complete_mask = 0;
  master->on_complete = completion;
  master->user_data = user_data;

  finish_read_ops(call);
  early_out_write_ops(call);

  return GRPC_CALL_OK;
}

grpc_call_error grpc_call_start_ioreq_and_call_back(
    grpc_call *call, const grpc_ioreq *reqs, size_t nreqs,
    grpc_ioreq_completion_func on_complete, void *user_data) {
  grpc_call_error err;
  lock(call);
  err = start_ioreq(call, reqs, nreqs, on_complete, user_data);
  unlock(call);
  return err;
}

void grpc_call_destroy(grpc_call *c) {
  int cancel;
  lock(c);
  if (c->have_alarm) {
    grpc_alarm_cancel(&c->alarm);
    c->have_alarm = 0;
  }
  cancel = c->read_state != READ_STATE_STREAM_CLOSED;
  unlock(c);
  if (cancel) grpc_call_cancel(c);
  GRPC_CALL_INTERNAL_UNREF(c, "destroy", 1);
}

grpc_call_error grpc_call_cancel(grpc_call *call) {
  return grpc_call_cancel_with_status(call, GRPC_STATUS_CANCELLED, "Cancelled");
}

grpc_call_error grpc_call_cancel_with_status(grpc_call *c,
                                             grpc_status_code status,
                                             const char *description) {
  grpc_transport_op op;
  grpc_mdstr *details =
      description ? grpc_mdstr_from_string(c->metadata_context, description)
                  : NULL;
  memset(&op, 0, sizeof(op));
  op.cancel_with_status = status;

  lock(c);
  set_status_code(c, STATUS_FROM_API_OVERRIDE, status);
  set_status_details(c, STATUS_FROM_API_OVERRIDE, details);
  unlock(c);

  execute_op(c, &op);

  return GRPC_CALL_OK;
}

static void execute_op(grpc_call *call, grpc_transport_op *op) {
  grpc_call_element *elem;
  elem = CALL_ELEM_FROM_CALL(call, 0);
  elem->filter->start_transport_op(elem, op);
}

grpc_call *grpc_call_from_top_element(grpc_call_element *elem) {
  return CALL_FROM_TOP_ELEM(elem);
}

static void call_alarm(void *arg, int success) {
  grpc_call *call = arg;
  if (success) {
    if (call->is_client) {
      grpc_call_cancel_with_status(call, GRPC_STATUS_DEADLINE_EXCEEDED,
                                   "Deadline Exceeded");
    } else {
      grpc_call_cancel(call);
    }
  }
  GRPC_CALL_INTERNAL_UNREF(call, "alarm", 1);
}

static void set_deadline_alarm(grpc_call *call, gpr_timespec deadline) {
  if (call->have_alarm) {
    gpr_log(GPR_ERROR, "Attempt to set deadline alarm twice");
    assert(0);
    return;
  }
  GRPC_CALL_INTERNAL_REF(call, "alarm");
  call->have_alarm = 1;
  grpc_alarm_init(&call->alarm, deadline, call_alarm, call, gpr_now());
}

/* we offset status by a small amount when storing it into transport metadata
   as metadata cannot store a 0 value (which is used as OK for grpc_status_codes
   */
#define STATUS_OFFSET 1
static void destroy_status(void *ignored) {}

static gpr_uint32 decode_status(grpc_mdelem *md) {
  gpr_uint32 status;
  void *user_data = grpc_mdelem_get_user_data(md, destroy_status);
  if (user_data) {
    status = ((gpr_uint32)(gpr_intptr)user_data) - STATUS_OFFSET;
  } else {
    if (!gpr_parse_bytes_to_uint32(grpc_mdstr_as_c_string(md->value),
                                   GPR_SLICE_LENGTH(md->value->slice),
                                   &status)) {
      status = GRPC_STATUS_UNKNOWN; /* could not parse status code */
    }
    grpc_mdelem_set_user_data(md, destroy_status,
                              (void *)(gpr_intptr)(status + STATUS_OFFSET));
  }
  return status;
}

static void recv_metadata(grpc_call *call, grpc_metadata_batch *md) {
  grpc_linked_mdelem *l;
  grpc_metadata_array *dest;
  grpc_metadata *mdusr;
  int is_trailing;
  grpc_mdctx *mdctx = call->metadata_context;

  is_trailing = call->read_state >= READ_STATE_GOT_INITIAL_METADATA;
  for (l = md->list.head; l != NULL; l = l->next) {
    grpc_mdelem *md = l->md;
    grpc_mdstr *key = md->key;
    if (key == grpc_channel_get_status_string(call->channel)) {
      set_status_code(call, STATUS_FROM_WIRE, decode_status(md));
    } else if (key == grpc_channel_get_message_string(call->channel)) {
      set_status_details(call, STATUS_FROM_WIRE, grpc_mdstr_ref(md->value));
    } else {
      dest = &call->buffered_metadata[is_trailing];
      if (dest->count == dest->capacity) {
        dest->capacity = GPR_MAX(dest->capacity + 8, dest->capacity * 2);
        dest->metadata =
            gpr_realloc(dest->metadata, sizeof(grpc_metadata) * dest->capacity);
      }
      mdusr = &dest->metadata[dest->count++];
      mdusr->key = grpc_mdstr_as_c_string(md->key);
      mdusr->value = grpc_mdstr_as_c_string(md->value);
      mdusr->value_length = GPR_SLICE_LENGTH(md->value->slice);
      if (call->owned_metadata_count == call->owned_metadata_capacity) {
        call->owned_metadata_capacity =
            GPR_MAX(call->owned_metadata_capacity + 8,
                    call->owned_metadata_capacity * 2);
        call->owned_metadata =
            gpr_realloc(call->owned_metadata,
                        sizeof(grpc_mdelem *) * call->owned_metadata_capacity);
      }
      call->owned_metadata[call->owned_metadata_count++] = md;
      l->md = 0;
    }
  }
  if (gpr_time_cmp(md->deadline, gpr_inf_future) != 0) {
    set_deadline_alarm(call, md->deadline);
  }
  if (!is_trailing) {
    call->read_state = READ_STATE_GOT_INITIAL_METADATA;
  }

  grpc_mdctx_lock(mdctx);
  for (l = md->list.head; l; l = l->next) {
    if (l->md) grpc_mdctx_locked_mdelem_unref(mdctx, l->md);
  }
  for (l = md->garbage.head; l; l = l->next) {
    grpc_mdctx_locked_mdelem_unref(mdctx, l->md);
  }
  grpc_mdctx_unlock(mdctx);
}

grpc_call_stack *grpc_call_get_call_stack(grpc_call *call) {
  return CALL_STACK_FROM_CALL(call);
}

/*
 * BATCH API IMPLEMENTATION
 */

static void set_status_value_directly(grpc_status_code status, void *dest) {
  *(grpc_status_code *)dest = status;
}

static void set_cancelled_value(grpc_status_code status, void *dest) {
  *(grpc_status_code *)dest = (status != GRPC_STATUS_OK);
}

static void finish_batch(grpc_call *call, grpc_op_error result, void *tag) {
  grpc_cq_end_op(call->cq, tag, call, do_nothing, NULL, GRPC_OP_OK);
}

grpc_call_error grpc_call_start_batch(grpc_call *call, const grpc_op *ops,
                                      size_t nops, void *tag) {
  grpc_ioreq reqs[GRPC_IOREQ_OP_COUNT];
  size_t in;
  size_t out;
  const grpc_op *op;
  grpc_ioreq *req;

  GRPC_CALL_LOG_BATCH(GPR_INFO, call, ops, nops, tag);

  if (nops == 0) {
    grpc_cq_begin_op(call->cq, call, GRPC_OP_COMPLETE);
    grpc_cq_end_op(call->cq, tag, call, do_nothing, NULL, GRPC_OP_OK);
    return GRPC_CALL_OK;
  }

  /* rewrite batch ops into ioreq ops */
  for (in = 0, out = 0; in < nops; in++) {
    op = &ops[in];
    switch (op->op) {
      case GRPC_OP_SEND_INITIAL_METADATA:
        req = &reqs[out++];
        req->op = GRPC_IOREQ_SEND_INITIAL_METADATA;
        req->data.send_metadata.count = op->data.send_initial_metadata.count;
        req->data.send_metadata.metadata =
            op->data.send_initial_metadata.metadata;
        break;
      case GRPC_OP_SEND_MESSAGE:
        req = &reqs[out++];
        req->op = GRPC_IOREQ_SEND_MESSAGE;
        req->data.send_message = op->data.send_message;
        break;
      case GRPC_OP_SEND_CLOSE_FROM_CLIENT:
        if (!call->is_client) {
          return GRPC_CALL_ERROR_NOT_ON_SERVER;
        }
        req = &reqs[out++];
        req->op = GRPC_IOREQ_SEND_CLOSE;
        break;
      case GRPC_OP_SEND_STATUS_FROM_SERVER:
        if (call->is_client) {
          return GRPC_CALL_ERROR_NOT_ON_CLIENT;
        }
        req = &reqs[out++];
        req->op = GRPC_IOREQ_SEND_TRAILING_METADATA;
        req->data.send_metadata.count =
            op->data.send_status_from_server.trailing_metadata_count;
        req->data.send_metadata.metadata =
            op->data.send_status_from_server.trailing_metadata;
        req = &reqs[out++];
        req->op = GRPC_IOREQ_SEND_STATUS;
        req->data.send_status.code = op->data.send_status_from_server.status;
        req->data.send_status.details =
            op->data.send_status_from_server.status_details;
        req = &reqs[out++];
        req->op = GRPC_IOREQ_SEND_CLOSE;
        break;
      case GRPC_OP_RECV_INITIAL_METADATA:
        if (!call->is_client) {
          return GRPC_CALL_ERROR_NOT_ON_SERVER;
        }
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_INITIAL_METADATA;
        req->data.recv_metadata = op->data.recv_initial_metadata;
        break;
      case GRPC_OP_RECV_MESSAGE:
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_MESSAGE;
        req->data.recv_message = op->data.recv_message;
        break;
      case GRPC_OP_RECV_STATUS_ON_CLIENT:
        if (!call->is_client) {
          return GRPC_CALL_ERROR_NOT_ON_SERVER;
        }
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_STATUS;
        req->data.recv_status.set_value = set_status_value_directly;
        req->data.recv_status.user_data = op->data.recv_status_on_client.status;
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_STATUS_DETAILS;
        req->data.recv_status_details.details =
            op->data.recv_status_on_client.status_details;
        req->data.recv_status_details.details_capacity =
            op->data.recv_status_on_client.status_details_capacity;
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_TRAILING_METADATA;
        req->data.recv_metadata =
            op->data.recv_status_on_client.trailing_metadata;
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_CLOSE;
        break;
      case GRPC_OP_RECV_CLOSE_ON_SERVER:
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_STATUS;
        req->data.recv_status.set_value = set_cancelled_value;
        req->data.recv_status.user_data =
            op->data.recv_close_on_server.cancelled;
        req = &reqs[out++];
        req->op = GRPC_IOREQ_RECV_CLOSE;
        break;
    }
  }

  grpc_cq_begin_op(call->cq, call, GRPC_OP_COMPLETE);

  return grpc_call_start_ioreq_and_call_back(call, reqs, out, finish_batch,
                                             tag);
}
