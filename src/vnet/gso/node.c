/*
 * Copyright (c) 2018 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vppinfra/error.h>
#include <vnet/ethernet/ethernet.h>
#include <vnet/feature/feature.h>
#include <vnet/gso/gso.h>
#include <vnet/ip/icmp46_packet.h>
#include <vnet/ip/ip4.h>
#include <vnet/ip/ip6.h>
#include <vnet/udp/udp_packet.h>

typedef struct
{
  u32 flags;
  u16 gso_size;
  u8 gso_l4_hdr_sz;
} gso_trace_t;

static u8 *
format_gso_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  gso_trace_t *t = va_arg (*args, gso_trace_t *);

  if (t->flags & VNET_BUFFER_F_GSO)
    {
      s = format (s, "gso_sz %d gso_l4_hdr_sz %d",
		  t->gso_size, t->gso_l4_hdr_sz);
    }

  return s;
}

static_always_inline u16
tso_alloc_tx_bufs (vlib_main_t * vm,
		   vnet_interface_per_thread_data_t * ptd,
		   vlib_buffer_t * b0, u32 n_bytes_b0, u16 l234_sz,
		   u16 gso_size)
{
  u16 size =
    clib_min (gso_size, vlib_buffer_get_default_data_size (vm) - l234_sz);

  /* rounded-up division */
  u16 n_bufs = (n_bytes_b0 - l234_sz + (size - 1)) / size;
  u16 n_alloc;

  ASSERT (n_bufs > 0);
  vec_validate (ptd->split_buffers, n_bufs - 1);

  n_alloc = vlib_buffer_alloc (vm, ptd->split_buffers, n_bufs);
  if (n_alloc < n_bufs)
    {
      vlib_buffer_free (vm, ptd->split_buffers, n_alloc);
      return 0;
    }
  return n_alloc;
}

static_always_inline void
tso_init_buf_from_template_base (vlib_buffer_t * nb0, vlib_buffer_t * b0,
				 u32 flags, u16 length)
{
  nb0->current_data = b0->current_data;
  nb0->total_length_not_including_first_buffer = 0;
  nb0->flags = VLIB_BUFFER_TOTAL_LENGTH_VALID | flags;
  nb0->trace_handle = b0->trace_handle;
  clib_memcpy_fast (&nb0->opaque, &b0->opaque, sizeof (nb0->opaque));
  clib_memcpy_fast (vlib_buffer_get_current (nb0),
		    vlib_buffer_get_current (b0), length);
  nb0->current_length = length;
}

static_always_inline void
tso_init_buf_from_template (vlib_main_t * vm, vlib_buffer_t * nb0,
			    vlib_buffer_t * b0, u16 template_data_sz,
			    u16 gso_size, u8 ** p_dst_ptr, u16 * p_dst_left,
			    u32 next_tcp_seq, u32 flags,
			    gso_header_offset_t * gho)
{
  tso_init_buf_from_template_base (nb0, b0, flags, template_data_sz);

  *p_dst_left =
    clib_min (gso_size,
	      vlib_buffer_get_default_data_size (vm) - (template_data_sz +
							nb0->current_data));
  *p_dst_ptr = vlib_buffer_get_current (nb0) + template_data_sz;

  tcp_header_t *tcp =
    (tcp_header_t *) (vlib_buffer_get_current (nb0) + gho->l4_hdr_offset);
  tcp->seq_number = clib_host_to_net_u32 (next_tcp_seq);
}

static_always_inline void
tso_fixup_segmented_buf (vlib_buffer_t * b0, u8 tcp_flags, int is_ip6,
			 gso_header_offset_t * gho)
{
  ip4_header_t *ip4 =
    (ip4_header_t *) (vlib_buffer_get_current (b0) + gho->l3_hdr_offset);
  ip6_header_t *ip6 =
    (ip6_header_t *) (vlib_buffer_get_current (b0) + gho->l3_hdr_offset);
  tcp_header_t *tcp =
    (tcp_header_t *) (vlib_buffer_get_current (b0) + gho->l4_hdr_offset);

  tcp->flags = tcp_flags;

  if (is_ip6)
    ip6->payload_length =
      clib_host_to_net_u16 (b0->current_length -
			    (gho->l4_hdr_offset - gho->l2_hdr_offset));
  else
    ip4->length =
      clib_host_to_net_u16 (b0->current_length -
			    (gho->l3_hdr_offset - gho->l2_hdr_offset));
}

/**
 * Allocate the necessary number of ptd->split_buffers,
 * and segment the possibly chained buffer(s) from b0 into
 * there.
 *
 * Return the cumulative number of bytes sent or zero
 * if allocation failed.
 */

static_always_inline u32
tso_segment_buffer (vlib_main_t * vm, vnet_interface_per_thread_data_t * ptd,
		    u32 sbi0, vlib_buffer_t * sb0, gso_header_offset_t * gho,
		    u32 n_bytes_b0, int is_ip6)
{
  u32 n_tx_bytes = 0;
  u16 gso_size = vnet_buffer2 (sb0)->gso_size;

  int l4_hdr_sz = gho->l4_hdr_sz;
  u8 save_tcp_flags = 0;
  u8 tcp_flags_no_fin_psh = 0;
  u32 next_tcp_seq = 0;

  tcp_header_t *tcp =
    (tcp_header_t *) (vlib_buffer_get_current (sb0) + gho->l4_hdr_offset);
  next_tcp_seq = clib_net_to_host_u32 (tcp->seq_number);
  /* store original flags for last packet and reset FIN and PSH */
  save_tcp_flags = tcp->flags;
  tcp_flags_no_fin_psh = tcp->flags & ~(TCP_FLAG_FIN | TCP_FLAG_PSH);
  tcp->checksum = 0;

  u32 default_bflags =
    sb0->flags & ~(VNET_BUFFER_F_GSO | VLIB_BUFFER_NEXT_PRESENT);
  u16 l234_sz = gho->l4_hdr_offset + l4_hdr_sz - gho->l2_hdr_offset;
  int first_data_size = clib_min (gso_size, sb0->current_length - l234_sz);
  next_tcp_seq += first_data_size;

  if (PREDICT_FALSE
      (!tso_alloc_tx_bufs (vm, ptd, sb0, n_bytes_b0, l234_sz, gso_size)))
    return 0;

  vlib_buffer_t *b0 = vlib_get_buffer (vm, ptd->split_buffers[0]);
  tso_init_buf_from_template_base (b0, sb0, default_bflags,
				   l234_sz + first_data_size);

  u32 total_src_left = n_bytes_b0 - l234_sz - first_data_size;
  if (total_src_left)
    {
      /* Need to copy more segments */
      u8 *src_ptr, *dst_ptr;
      u16 src_left, dst_left;
      /* current source buffer */
      vlib_buffer_t *csb0 = sb0;
      u32 csbi0 = sbi0;
      /* current dest buffer */
      vlib_buffer_t *cdb0;
      u16 dbi = 1;		/* the buffer [0] is b0 */

      src_ptr = vlib_buffer_get_current (sb0) + l234_sz + first_data_size;
      src_left = sb0->current_length - l234_sz - first_data_size;

      tso_fixup_segmented_buf (b0, tcp_flags_no_fin_psh, is_ip6, gho);

      /* grab a second buffer and prepare the loop */
      ASSERT (dbi < vec_len (ptd->split_buffers));
      cdb0 = vlib_get_buffer (vm, ptd->split_buffers[dbi++]);
      tso_init_buf_from_template (vm, cdb0, b0, l234_sz, gso_size, &dst_ptr,
				  &dst_left, next_tcp_seq, default_bflags,
				  gho);

      /* an arbitrary large number to catch the runaway loops */
      int nloops = 2000;
      while (total_src_left)
	{
	  if (nloops-- <= 0)
	    clib_panic ("infinite loop detected");
	  u16 bytes_to_copy = clib_min (src_left, dst_left);

	  clib_memcpy_fast (dst_ptr, src_ptr, bytes_to_copy);

	  src_left -= bytes_to_copy;
	  src_ptr += bytes_to_copy;
	  total_src_left -= bytes_to_copy;
	  dst_left -= bytes_to_copy;
	  dst_ptr += bytes_to_copy;
	  next_tcp_seq += bytes_to_copy;
	  cdb0->current_length += bytes_to_copy;

	  if (0 == src_left)
	    {
	      int has_next = (csb0->flags & VLIB_BUFFER_NEXT_PRESENT);
	      u32 next_bi = csb0->next_buffer;

	      /* init src to the next buffer in chain */
	      if (has_next)
		{
		  csbi0 = next_bi;
		  csb0 = vlib_get_buffer (vm, csbi0);
		  src_left = csb0->current_length;
		  src_ptr = vlib_buffer_get_current (csb0);
		}
	      else
		{
		  ASSERT (total_src_left == 0);
		  break;
		}
	    }
	  if (0 == dst_left && total_src_left)
	    {
	      n_tx_bytes += cdb0->current_length;
	      ASSERT (dbi < vec_len (ptd->split_buffers));
	      cdb0 = vlib_get_buffer (vm, ptd->split_buffers[dbi++]);
	      tso_init_buf_from_template (vm, cdb0, b0, l234_sz,
					  gso_size, &dst_ptr, &dst_left,
					  next_tcp_seq, default_bflags, gho);
	    }
	}

      tso_fixup_segmented_buf (cdb0, save_tcp_flags, is_ip6, gho);

      n_tx_bytes += cdb0->current_length;
    }
  n_tx_bytes += b0->current_length;
  return n_tx_bytes;
}

static_always_inline void
drop_one_buffer_and_count (vlib_main_t * vm, vnet_main_t * vnm,
			   vlib_node_runtime_t * node, u32 * pbi0,
			   u32 sw_if_index, u32 drop_error_code)
{
  u32 thread_index = vm->thread_index;

  vlib_simple_counter_main_t *cm;
  cm =
    vec_elt_at_index (vnm->interface_main.sw_if_counters,
		      VNET_INTERFACE_COUNTER_TX_ERROR);
  vlib_increment_simple_counter (cm, thread_index, sw_if_index, 1);

  vlib_error_drop_buffers (vm, node, pbi0,
			   /* buffer stride */ 1,
			   /* n_buffers */ 1,
			   VNET_INTERFACE_OUTPUT_NEXT_DROP,
			   node->node_index, drop_error_code);
}

static_always_inline uword
vnet_gso_node_inline (vlib_main_t * vm,
		      vlib_node_runtime_t * node,
		      vlib_frame_t * frame,
		      vnet_main_t * vnm,
		      vnet_hw_interface_t * hi,
		      int is_ip6, int do_segmentation)
{
  u32 *to_next;
  u32 next_index = node->cached_next_index;
  u32 *from = vlib_frame_vector_args (frame);
  u32 n_left_from = frame->n_vectors;
  u32 *from_end = from + n_left_from;
  u32 thread_index = vm->thread_index;
  vnet_interface_main_t *im = &vnm->interface_main;
  vnet_interface_per_thread_data_t *ptd =
    vec_elt_at_index (im->per_thread_data, thread_index);
  vlib_buffer_t *bufs[VLIB_FRAME_SIZE], **b = bufs;

  vlib_get_buffers (vm, from, b, n_left_from);

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (from + 8 <= from_end && n_left_to_next >= 4)
	{
	  u32 bi0, bi1, bi2, bi3;
	  u32 next0, next1, next2, next3;
	  u32 swif0, swif1, swif2, swif3;
	  gso_trace_t *t0, *t1, *t2, *t3;
	  vnet_hw_interface_t *hi0, *hi1, *hi2, *hi3;

	  /* Prefetch next iteration. */
	  vlib_prefetch_buffer_header (b[4], LOAD);
	  vlib_prefetch_buffer_header (b[5], LOAD);
	  vlib_prefetch_buffer_header (b[6], LOAD);
	  vlib_prefetch_buffer_header (b[7], LOAD);

	  bi0 = from[0];
	  bi1 = from[1];
	  bi2 = from[2];
	  bi3 = from[3];
	  to_next[0] = bi0;
	  to_next[1] = bi1;
	  to_next[2] = bi2;
	  to_next[3] = bi3;

	  swif0 = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
	  swif1 = vnet_buffer (b[1])->sw_if_index[VLIB_TX];
	  swif2 = vnet_buffer (b[2])->sw_if_index[VLIB_TX];
	  swif3 = vnet_buffer (b[3])->sw_if_index[VLIB_TX];

	  if (PREDICT_FALSE (hi->sw_if_index != swif0))
	    {
	      hi0 = vnet_get_sup_hw_interface (vnm, swif0);
	      if ((hi0->flags & VNET_HW_INTERFACE_FLAG_SUPPORTS_GSO) == 0 &&
		  (b[0]->flags & VNET_BUFFER_F_GSO))
		break;
	    }
	  if (PREDICT_FALSE (hi->sw_if_index != swif1))
	    {
	      hi1 = vnet_get_sup_hw_interface (vnm, swif0);
	      if (!(hi1->flags & VNET_HW_INTERFACE_FLAG_SUPPORTS_GSO) &&
		  (b[1]->flags & VNET_BUFFER_F_GSO))
		break;
	    }
	  if (PREDICT_FALSE (hi->sw_if_index != swif2))
	    {
	      hi2 = vnet_get_sup_hw_interface (vnm, swif0);
	      if ((hi2->flags & VNET_HW_INTERFACE_FLAG_SUPPORTS_GSO) == 0 &&
		  (b[2]->flags & VNET_BUFFER_F_GSO))
		break;
	    }
	  if (PREDICT_FALSE (hi->sw_if_index != swif3))
	    {
	      hi3 = vnet_get_sup_hw_interface (vnm, swif0);
	      if (!(hi3->flags & VNET_HW_INTERFACE_FLAG_SUPPORTS_GSO) &&
		  (b[3]->flags & VNET_BUFFER_F_GSO))
		break;
	    }

	  if (b[0]->flags & VLIB_BUFFER_IS_TRACED)
	    {
	      t0 = vlib_add_trace (vm, node, b[0], sizeof (t0[0]));
	      t0->flags = b[0]->flags & VNET_BUFFER_F_GSO;
	      t0->gso_size = vnet_buffer2 (b[0])->gso_size;
	      t0->gso_l4_hdr_sz = vnet_buffer2 (b[0])->gso_l4_hdr_sz;
	    }
	  if (b[1]->flags & VLIB_BUFFER_IS_TRACED)
	    {
	      t1 = vlib_add_trace (vm, node, b[1], sizeof (t1[0]));
	      t1->flags = b[1]->flags & VNET_BUFFER_F_GSO;
	      t1->gso_size = vnet_buffer2 (b[1])->gso_size;
	      t1->gso_l4_hdr_sz = vnet_buffer2 (b[1])->gso_l4_hdr_sz;
	    }
	  if (b[2]->flags & VLIB_BUFFER_IS_TRACED)
	    {
	      t2 = vlib_add_trace (vm, node, b[2], sizeof (t2[0]));
	      t2->flags = b[2]->flags & VNET_BUFFER_F_GSO;
	      t2->gso_size = vnet_buffer2 (b[2])->gso_size;
	      t2->gso_l4_hdr_sz = vnet_buffer2 (b[2])->gso_l4_hdr_sz;
	    }
	  if (b[3]->flags & VLIB_BUFFER_IS_TRACED)
	    {
	      t3 = vlib_add_trace (vm, node, b[3], sizeof (t3[0]));
	      t3->flags = b[3]->flags & VNET_BUFFER_F_GSO;
	      t3->gso_size = vnet_buffer2 (b[3])->gso_size;
	      t3->gso_l4_hdr_sz = vnet_buffer2 (b[3])->gso_l4_hdr_sz;
	    }

	  from += 4;
	  to_next += 4;
	  n_left_to_next -= 4;
	  n_left_from -= 4;

	  next0 = next1 = 0;
	  next2 = next3 = 0;
	  vnet_feature_next (&next0, b[0]);
	  vnet_feature_next (&next1, b[1]);
	  vnet_feature_next (&next2, b[2]);
	  vnet_feature_next (&next3, b[3]);
	  vlib_validate_buffer_enqueue_x4 (vm, node, next_index, to_next,
					   n_left_to_next, bi0, bi1, bi2, bi3,
					   next0, next1, next2, next3);
	  b += 4;
	}

      while (from + 1 <= from_end && n_left_to_next > 0)
	{
	  u32 bi0, swif0;
	  gso_trace_t *t0;
	  vnet_hw_interface_t *hi0;
	  u32 next0 = 0;

	  swif0 = vnet_buffer (b[0])->sw_if_index[VLIB_TX];
	  if (PREDICT_FALSE (hi->sw_if_index != swif0))
	    {
	      hi0 = vnet_get_sup_hw_interface (vnm, swif0);
	      if ((hi0->flags & VNET_HW_INTERFACE_FLAG_SUPPORTS_GSO) == 0 &&
		  (b[0]->flags & VNET_BUFFER_F_GSO))
		do_segmentation = 1;
	    }

	  /* speculatively enqueue b0 to the current next frame */
	  to_next[0] = bi0 = from[0];
	  to_next += 1;
	  n_left_to_next -= 1;
	  from += 1;
	  n_left_from -= 1;

	  if (b[0]->flags & VLIB_BUFFER_IS_TRACED)
	    {
	      t0 = vlib_add_trace (vm, node, b[0], sizeof (t0[0]));
	      t0->flags = b[0]->flags & VNET_BUFFER_F_GSO;
	      t0->gso_size = vnet_buffer2 (b[0])->gso_size;
	      t0->gso_l4_hdr_sz = vnet_buffer2 (b[0])->gso_l4_hdr_sz;
	    }

	  if (do_segmentation)
	    {
	      if (PREDICT_FALSE (b[0]->flags & VNET_BUFFER_F_GSO))
		{
		  /*
		   * Undo the enqueue of the b0 - it is not going anywhere,
		   * and will be freed either after it's segmented or
		   * when dropped, if there is no buffers to segment into.
		   */
		  to_next -= 1;
		  n_left_to_next += 1;
		  /* undo the counting. */
		  gso_header_offset_t gho;
		  u32 n_bytes_b0 = vlib_buffer_length_in_chain (vm, b[0]);
		  u32 n_tx_bytes = 0;

		  gho = vnet_gso_header_offset_parser (b[0], is_ip6);
		  n_tx_bytes =
		    tso_segment_buffer (vm, ptd, bi0, b[0], &gho, n_bytes_b0,
					is_ip6);

		  if (PREDICT_FALSE (n_tx_bytes == 0))
		    {
		      drop_one_buffer_and_count (vm, vnm, node, from - 1,
						 hi->sw_if_index,
						 VNET_INTERFACE_OUTPUT_ERROR_NO_BUFFERS_FOR_GSO);
		      b += 1;
		      continue;
		    }

		  u16 n_tx_bufs = vec_len (ptd->split_buffers);
		  u32 *from_seg = ptd->split_buffers;

		  while (n_tx_bufs > 0)
		    {
		      u32 sbi0;
		      vlib_buffer_t *sb0;
		      if (n_tx_bufs >= n_left_to_next)
			{
			  while (n_left_to_next > 0)
			    {
			      sbi0 = to_next[0] = from_seg[0];
			      sb0 = vlib_get_buffer (vm, sbi0);
			      to_next += 1;
			      from_seg += 1;
			      n_left_to_next -= 1;
			      n_tx_bufs -= 1;
			      vnet_feature_next (&next0, sb0);
			      vlib_validate_buffer_enqueue_x1 (vm, node,
							       next_index,
							       to_next,
							       n_left_to_next,
							       sbi0, next0);
			    }
			  vlib_put_next_frame (vm, node, next_index,
					       n_left_to_next);
			  vlib_get_new_next_frame (vm, node, next_index,
						   to_next, n_left_to_next);
			}
		      while (n_tx_bufs > 0)
			{
			  sbi0 = to_next[0] = from_seg[0];
			  sb0 = vlib_get_buffer (vm, sbi0);
			  to_next += 1;
			  from_seg += 1;
			  n_left_to_next -= 1;
			  n_tx_bufs -= 1;
			  vnet_feature_next (&next0, sb0);
			  vlib_validate_buffer_enqueue_x1 (vm, node,
							   next_index,
							   to_next,
							   n_left_to_next,
							   sbi0, next0);
			}
		    }
		  /* The buffers were enqueued. Reset the length */
		  _vec_len (ptd->split_buffers) = 0;
		  /* Free the now segmented buffer */
		  vlib_buffer_free_one (vm, bi0);
		  b += 1;
		  continue;
		}
	    }

	  vnet_feature_next (&next0, b[0]);
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index, to_next,
					   n_left_to_next, bi0, next0);
	  b += 1;
	}
      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  return frame->n_vectors;
}

static_always_inline uword
vnet_gso_inline (vlib_main_t * vm,
		 vlib_node_runtime_t * node, vlib_frame_t * frame, int is_ip6)
{
  vnet_main_t *vnm = vnet_get_main ();
  vnet_hw_interface_t *hi;

  if (frame->n_vectors > 0)
    {
      u32 *from = vlib_frame_vector_args (frame);
      vlib_buffer_t *b = vlib_get_buffer (vm, from[0]);
      hi = vnet_get_sup_hw_interface (vnm,
				      vnet_buffer (b)->sw_if_index[VLIB_TX]);
      /*
       * The 3-headed "if" is here because we want to err on the side
       * of not impacting the non-GSO performance - so for the more
       * common case of no GSO interfaces we want to prevent the
       * segmentation codepath from being there altogether.
       */
      if (hi->flags & VNET_HW_INTERFACE_FLAG_SUPPORTS_GSO)
	return vnet_gso_node_inline (vm, node, frame, vnm, hi,
				     is_ip6, /* do_segmentation */ 0);
      else
	return vnet_gso_node_inline (vm, node, frame, vnm, hi,
				     is_ip6, /* do_segmentation */ 1);
    }
  return 0;
}

VLIB_NODE_FN (gso_l2_ip4_node) (vlib_main_t * vm, vlib_node_runtime_t * node,
				vlib_frame_t * frame)
{
  return vnet_gso_inline (vm, node, frame, 0 /* ip6 */ );
}

VLIB_NODE_FN (gso_l2_ip6_node) (vlib_main_t * vm, vlib_node_runtime_t * node,
				vlib_frame_t * frame)
{
  return vnet_gso_inline (vm, node, frame, 1 /* ip6 */ );
}

VLIB_NODE_FN (gso_ip4_node) (vlib_main_t * vm, vlib_node_runtime_t * node,
			     vlib_frame_t * frame)
{
  return vnet_gso_inline (vm, node, frame, 0 /* ip6 */ );
}

VLIB_NODE_FN (gso_ip6_node) (vlib_main_t * vm, vlib_node_runtime_t * node,
			     vlib_frame_t * frame)
{
  return vnet_gso_inline (vm, node, frame, 1 /* ip6 */ );
}

/* *INDENT-OFF* */

VLIB_REGISTER_NODE (gso_l2_ip4_node) = {
  .vector_size = sizeof (u32),
  .format_trace = format_gso_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = 0,
  .n_next_nodes = 0,
  .name = "gso-l2-ip4",
};

VLIB_REGISTER_NODE (gso_l2_ip6_node) = {
  .vector_size = sizeof (u32),
  .format_trace = format_gso_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = 0,
  .n_next_nodes = 0,
  .name = "gso-l2-ip6",
};

VLIB_REGISTER_NODE (gso_ip4_node) = {
  .vector_size = sizeof (u32),
  .format_trace = format_gso_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = 0,
  .n_next_nodes = 0,
  .name = "gso-ip4",
};

VLIB_REGISTER_NODE (gso_ip6_node) = {
  .vector_size = sizeof (u32),
  .format_trace = format_gso_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = 0,
  .n_next_nodes = 0,
  .name = "gso-ip6",
};

VNET_FEATURE_INIT (gso_l2_ip4_node, static) = {
  .arc_name = "l2-output-ip4",
  .node_name = "gso-l2-ip4",
  .runs_before = VNET_FEATURES ("l2-output-feat-arc-end"),
};

VNET_FEATURE_INIT (gso_l2_ip6_node, static) = {
  .arc_name = "l2-output-ip6",
  .node_name = "gso-l2-ip6",
  .runs_before = VNET_FEATURES ("l2-output-feat-arc-end"),
};

VNET_FEATURE_INIT (gso_ip4_node, static) = {
  .arc_name = "ip4-output",
  .node_name = "gso-ip4",
  .runs_after = VNET_FEATURES ("ipsec4-output-feature"),
  .runs_before = VNET_FEATURES ("interface-output"),
};

VNET_FEATURE_INIT (gso_ip6_node, static) = {
  .arc_name = "ip6-output",
  .node_name = "gso-ip6",
  .runs_after = VNET_FEATURES ("ipsec6-output-feature"),
  .runs_before = VNET_FEATURES ("interface-output"),
};

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
