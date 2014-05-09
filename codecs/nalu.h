/*****************************************************************************
 * nalu.h:
 *****************************************************************************
 * Copyright (C) 2013-2014 L-SMASH project
 *
 * Authors: Yusuke Nakamura <muken.the.vfrmaniac@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *****************************************************************************/

/* This file is available under an ISC license. */

static inline uint64_t nalu_get_codeNum
(
    lsmash_bits_t *bits
)
{
    uint32_t leadingZeroBits = 0;
    for( int b = 0; !b; leadingZeroBits++ )
        b = lsmash_bits_get( bits, 1 );
    --leadingZeroBits;
    return ((uint64_t)1 << leadingZeroBits) - 1 + lsmash_bits_get( bits, leadingZeroBits );
}

static inline uint64_t nalu_decode_exp_golomb_ue
(
    uint64_t codeNum
)
{
    return codeNum;
}

static inline int64_t nalu_decode_exp_golomb_se
(
    uint64_t codeNum
)
{
    if( codeNum & 1 )
        return (int64_t)((codeNum >> 1) + 1);
    return -1 * (int64_t)(codeNum >> 1);
}

static inline uint64_t nalu_get_exp_golomb_ue
(
    lsmash_bits_t *bits
)
{
    uint64_t codeNum = nalu_get_codeNum( bits );
    return nalu_decode_exp_golomb_ue( codeNum );
}

static inline uint64_t nalu_get_exp_golomb_se
(
    lsmash_bits_t *bits
)
{
    uint64_t codeNum = nalu_get_codeNum( bits );
    return nalu_decode_exp_golomb_se( codeNum );
}

/* Convert EBSP (Encapsulated Byte Sequence Packets) to RBSP (Raw Byte Sequence Packets). */
static inline uint8_t *nalu_remove_emulation_prevention
(
    uint8_t *src,
    uint64_t src_length,
    uint8_t *dst
)
{
    uint8_t *src_end = src + src_length;
    while( src < src_end )
        if( ((src + 2) < src_end) && !src[0] && !src[1] && (src[2] == 0x03) )
        {
            /* 0x000003 -> 0x0000 */
            *dst++ = *src++;
            *dst++ = *src++;
            src++;  /* Skip emulation_prevention_three_byte (0x03). */
        }
        else
            *dst++ = *src++;
    return dst;
}

static inline int nalu_import_rbsp_from_ebsp
(
    lsmash_bits_t *bits,
    uint8_t       *rbsp_buffer,
    uint8_t       *ebsp,
    uint64_t       ebsp_size
)
{
    uint8_t *rbsp_start  = rbsp_buffer;
    uint8_t *rbsp_end    = nalu_remove_emulation_prevention( ebsp, ebsp_size, rbsp_buffer );
    uint64_t rbsp_length = rbsp_end - rbsp_start;
    return lsmash_bits_import_data( bits, rbsp_start, rbsp_length );
}

static inline int nalu_check_more_rbsp_data
(
    lsmash_bits_t *bits
)
{
    lsmash_bs_t *bs = bits->bs;
    lsmash_buffer_t *buffer = &bs->buffer;
    if( buffer->pos < buffer->store && !(bits->store == 0 && (buffer->store == buffer->pos + 1)) )
        return 1;       /* rbsp_trailing_bits will be placed at the next or later byte.
                         * Note: bs->pos points at the next byte if bits->store isn't empty. */
    if( bits->store == 0 )
    {
        if( buffer->store == buffer->pos + 1 )
            return buffer->data[ buffer->pos ] != 0x80;
        /* No rbsp_trailing_bits is present in RBSP data. */
        bs->error = 1;
        return 0;
    }
    /* Check whether remainder of bits is identical to rbsp_trailing_bits. */
    uint8_t remainder_bits = bits->cache & ~(~0U << bits->store);
    uint8_t rbsp_trailing_bits = 1U << (bits->store - 1);
    return remainder_bits != rbsp_trailing_bits;
}

static inline int nalu_get_max_ps_length
(
    lsmash_entry_list_t *ps_list,
    uint32_t            *max_ps_length
)
{
    *max_ps_length = 0;
    for( lsmash_entry_t *entry = ps_list->head; entry; entry = entry->next )
    {
        isom_dcr_ps_entry_t *ps = (isom_dcr_ps_entry_t *)entry->data;
        if( !ps )
            return -1;
        if( ps->unused )
            continue;
        *max_ps_length = LSMASH_MAX( *max_ps_length, ps->nalUnitLength );
    }
    return 0;
}

static inline int nalu_get_ps_count
(
    lsmash_entry_list_t *ps_list,
    uint32_t            *ps_count
)
{
    *ps_count = 0;
    for( lsmash_entry_t *entry = ps_list ? ps_list->head : NULL; entry; entry = entry->next )
    {
        isom_dcr_ps_entry_t *ps = (isom_dcr_ps_entry_t *)entry->data;
        if( !ps )
            return -1;
        if( ps->unused )
            continue;
        ++(*ps_count);
    }
    return 0;
}

static inline int nalu_check_same_ps_existence
(
    lsmash_entry_list_t *ps_list,
    void                *ps_data,
    uint32_t             ps_length
)
{
    for( lsmash_entry_t *entry = ps_list->head; entry; entry = entry->next )
    {
        isom_dcr_ps_entry_t *ps = (isom_dcr_ps_entry_t *)entry->data;
        if( !ps )
            return -1;
        if( ps->unused )
            continue;
        if( ps->nalUnitLength == ps_length && !memcmp( ps->nalUnit, ps_data, ps_length ) )
            return 1;   /* The same parameter set already exists. */
    }
    return 0;
}

static inline int nalu_get_dcr_ps
(
    lsmash_bs_t         *bs,
    lsmash_entry_list_t *list,
    uint8_t              entry_count
)
{
    for( uint8_t i = 0; i < entry_count; i++ )
    {
        isom_dcr_ps_entry_t *data = lsmash_malloc( sizeof(isom_dcr_ps_entry_t) );
        if( !data )
            return -1;
        if( lsmash_add_entry( list, data ) )
        {
            lsmash_free( data );
            return -1;
        }
        data->nalUnitLength = lsmash_bs_get_be16( bs );
        data->nalUnit       = lsmash_bs_get_bytes( bs, data->nalUnitLength );
        if( !data->nalUnit )
        {
            lsmash_remove_entries( list, isom_remove_dcr_ps );
            return -1;
        }
    }
    return 0;
}

static inline int nalu_check_next_short_start_code
(
    uint8_t *buf_pos,
    uint8_t *buf_end
)
{
    return ((buf_pos + 2) < buf_end) && !buf_pos[0] && !buf_pos[1] && (buf_pos[2] == 0x01);
}