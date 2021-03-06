/*****************************************************************************
 * webvtt.c: WEBVTT text demuxer (as ISO1446-30 payload)
 *****************************************************************************
 * Copyright (C) 2017 VideoLabs, VLC authors and VideoLAN
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_demux.h>
#include <vlc_memstream.h>

#include "../codec/webvtt/webvtt.h"

/*****************************************************************************
 * Prototypes:
 *****************************************************************************/

struct demux_sys_t
{
    es_out_id_t *es;
    bool         b_slave;
    bool         b_first_time;
    int          i_next_block_flags;
    mtime_t      i_next_demux_time;
    mtime_t      i_length;
    struct
    {
        void    *p_data;
        size_t   i_data;
    } regions_headers, styles_headers;

    struct
    {
        webvtt_cue_t *p_array;
        size_t  i_alloc;
        size_t  i_count;
        size_t  i_current;
    } cues;
};

static int Demux( demux_t * );
static int Control( demux_t *, int, va_list );

/*****************************************************************************
 *
 *****************************************************************************/
static int cue_Compare( const void *a, const void *b )
{
    const mtime_t diff = ((webvtt_cue_t *)a)->i_start - ((webvtt_cue_t *)b)->i_start;
    return (diff) ? diff / (( diff > 0 ) ? diff : -diff) : 0;
}

struct cue_searchkey
{
    webvtt_cue_t cue;
    webvtt_cue_t *p_last;
};

static int cue_Bsearch_Compare( const void *key, const void *other )
{
    struct cue_searchkey *p_key = (struct cue_searchkey *) key;
    webvtt_cue_t cue = *((webvtt_cue_t *) other);
    p_key->p_last = (webvtt_cue_t *) other;
    return cue_Compare( &p_key->cue, &cue );
}

static size_t cue_GetIndexByTime( demux_sys_t *p_sys, mtime_t i_time )
{
    size_t i_index = 0;
    if( p_sys->cues.p_array )
    {
        struct cue_searchkey key;
        key.cue.i_start = i_time;
        key.p_last = NULL;

        webvtt_cue_t *p_cue = bsearch( &key, p_sys->cues.p_array, p_sys->cues.i_count,
                                      sizeof(webvtt_cue_t), cue_Bsearch_Compare );
        if( p_cue )
            key.p_last = p_cue;

        i_index = (key.p_last - p_sys->cues.p_array);
        if( cue_Compare( key.p_last, &key ) < 0 )
            i_index++;
    }
    return i_index;
}

static block_t *ConvertWEBVTT( const webvtt_cue_t *p_cue, bool b_continued )
{
    struct vlc_memstream stream;

    if( vlc_memstream_open( &stream ) )
        return NULL;

    const size_t paylsize = 8 + strlen( p_cue->psz_text );
    const size_t idensize = (p_cue->psz_id) ? 8 + strlen( p_cue->psz_id ) : 0;
    const size_t attrsize = (p_cue->psz_attrs) ? 8 + strlen( p_cue->psz_attrs ) : 0;
    const size_t vttcsize = 8 + paylsize + attrsize + idensize;

    uint8_t vttcbox[8] = { 0, 0, 0, 0, 'v', 't', 't', 'c' };
    if( b_continued )
        vttcbox[7] = 'x';
    SetDWBE( vttcbox, vttcsize );
    vlc_memstream_write( &stream, vttcbox, 8 );

    if( p_cue->psz_id )
    {
        uint8_t idenbox[8] = { 0, 0, 0, 0, 'i', 'd', 'e', 'n' };
        SetDWBE( idenbox, idensize );
        vlc_memstream_write( &stream, idenbox, 8 );
        vlc_memstream_write( &stream, p_cue->psz_id, idensize - 8 );
    }

    if( p_cue->psz_attrs )
    {
        uint8_t attrbox[8] = { 0, 0, 0, 0, 's', 't', 't', 'g' };
        SetDWBE( attrbox, attrsize );
        vlc_memstream_write( &stream, attrbox, 8 );
        vlc_memstream_write( &stream, p_cue->psz_attrs, attrsize - 8 );
    }

    uint8_t paylbox[8] = { 0, 0, 0, 0, 'p', 'a', 'y', 'l' };
    SetDWBE( paylbox, paylsize );
    vlc_memstream_write( &stream, paylbox, 8 );
    vlc_memstream_write( &stream, p_cue->psz_text, paylsize - 8 );

    if( vlc_memstream_close( &stream ) == VLC_SUCCESS )
        return block_heap_Alloc( stream.ptr, stream.length );
    else
        return NULL;
}

static void memstream_Append( struct vlc_memstream *ms, const char *psz )
{
    if( ms->stream != NULL )
    {
        vlc_memstream_puts( ms, psz );
        vlc_memstream_putc( ms, '\n' );
    }
}

static void memstream_Grab( struct vlc_memstream *ms, void **pp, size_t *pi )
{
    if( ms->stream != NULL && vlc_memstream_close( ms ) == VLC_SUCCESS )
    {
        *pp = ms->ptr;
        *pi = ms->length;
    }
}

struct callback_ctx
{
    demux_t *p_demux;
    struct vlc_memstream regions, styles;
    bool b_ordered;
};

static webvtt_cue_t * ParserGetCueHandler( void *priv )
{
    struct callback_ctx *ctx = (struct callback_ctx *) priv;
    demux_sys_t *p_sys = ctx->p_demux->p_sys;
    if( p_sys->cues.i_alloc <= p_sys->cues.i_count )
    {
        webvtt_cue_t *p_realloc = realloc( p_sys->cues.p_array,
                sizeof( webvtt_cue_t ) * ( p_sys->cues.i_alloc + 64 ) );
        if( p_realloc )
        {
            p_sys->cues.p_array = p_realloc;
            p_sys->cues.i_alloc += 64;
        }
    }

    if( p_sys->cues.i_alloc > p_sys->cues.i_count )
        return &p_sys->cues.p_array[p_sys->cues.i_count++];

    return NULL;
}

static void ParserCueDoneHandler( void *priv, webvtt_cue_t *p_cue )
{
    struct callback_ctx *ctx = (struct callback_ctx *) priv;
    demux_sys_t *p_sys = ctx->p_demux->p_sys;
    if( p_cue->i_stop > p_sys->i_length )
        p_sys->i_length = p_cue->i_stop;
    if( p_sys->cues.i_count > 0 &&
        p_sys->cues.p_array[p_sys->cues.i_count - 1].i_start != p_cue->i_start )
        ctx->b_ordered = false;
}

static void ParserHeaderHandler( void *priv, enum webvtt_header_line_e s,
                                 bool b_new, const char *psz_line )
{
    VLC_UNUSED(b_new);
    struct callback_ctx *ctx = (struct callback_ctx *) priv;
    if( s == WEBVTT_HEADER_STYLE )
        memstream_Append( &ctx->styles, psz_line );
    else if( s == WEBVTT_HEADER_REGION )
        memstream_Append( &ctx->regions, psz_line );
}

static int ReadWEBVTT( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    struct callback_ctx ctx;
    ctx.p_demux = p_demux;
    ctx.b_ordered = true;

    webvtt_text_parser_t *p_parser =
            webvtt_text_parser_New( &ctx, ParserGetCueHandler,
                                          ParserCueDoneHandler,
                                          ParserHeaderHandler );
    if( p_parser == NULL )
        return VLC_EGENERIC;

    (void) vlc_memstream_open( &ctx.regions );
    (void) vlc_memstream_open( &ctx.styles );

    char *psz_line;
    while( (psz_line = vlc_stream_ReadLine( p_demux->s )) )
        webvtt_text_parser_Feed( p_parser, psz_line );
    webvtt_text_parser_Feed( p_parser, NULL );

    if( !ctx.b_ordered )
        qsort( p_sys->cues.p_array, p_sys->cues.i_count, sizeof(webvtt_cue_t), cue_Compare );

    memstream_Grab( &ctx.regions, &p_sys->regions_headers.p_data,
                                  &p_sys->regions_headers.i_data );
    memstream_Grab( &ctx.styles, &p_sys->styles_headers.p_data,
                                 &p_sys->styles_headers.i_data );

    webvtt_text_parser_Delete( p_parser );

    return VLC_SUCCESS;
}

static void MakeExtradata( demux_sys_t *p_sys, void **p_extra, size_t *pi_extra )
{
    struct vlc_memstream extradata;
    if( vlc_memstream_open( &extradata ) )
        return;
    vlc_memstream_puts( &extradata, "WEBVTT\n\n");
    vlc_memstream_write( &extradata, p_sys->regions_headers.p_data,
                                     p_sys->regions_headers.i_data );
    vlc_memstream_write( &extradata, p_sys->styles_headers.p_data,
                                     p_sys->styles_headers.i_data );
    memstream_Grab( &extradata, p_extra, pi_extra );
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    int64_t *pi64, i64;
    double *pf, f;

    switch( i_query )
    {
        case DEMUX_CAN_SEEK:
            *va_arg( args, bool * ) = true;
            return VLC_SUCCESS;

        case DEMUX_GET_LENGTH:
            *(va_arg( args, int64_t * )) = p_sys->i_length;
            return VLC_SUCCESS;

        case DEMUX_GET_TIME:
            pi64 = va_arg( args, int64_t * );
            *pi64 = p_sys->i_next_demux_time;
            return VLC_SUCCESS;

        case DEMUX_SET_TIME:
            i64 = va_arg( args, int64_t );
            {
                p_sys->cues.i_current = cue_GetIndexByTime( p_sys, i64 );
                p_sys->b_first_time = true;
                p_sys->i_next_demux_time =
                        p_sys->cues.p_array[p_sys->cues.i_current].i_start;
                p_sys->i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
                return VLC_SUCCESS;
            }

        case DEMUX_GET_POSITION:
            pf = va_arg( args, double * );
            if( p_sys->cues.i_current >= p_sys->cues.i_count )
            {
                *pf = 1.0;
            }
            else if( p_sys->cues.i_count > 0 )
            {
                *pf = (double) p_sys->i_next_demux_time /
                      (p_sys->i_length + 0.5);
            }
            else
            {
                *pf = 0.0;
            }
            return VLC_SUCCESS;

        case DEMUX_SET_POSITION:
            f = va_arg( args, double );
            if( p_sys->cues.i_count )
            {
                i64 = f * p_sys->i_length;
                p_sys->cues.i_current = cue_GetIndexByTime( p_sys, i64 );
                p_sys->b_first_time = true;
                p_sys->i_next_demux_time =
                        p_sys->cues.p_array[p_sys->cues.i_current].i_start;
                p_sys->i_next_block_flags |= BLOCK_FLAG_DISCONTINUITY;
                return VLC_SUCCESS;
            }
            break;

        case DEMUX_SET_NEXT_DEMUX_TIME:
            p_sys->b_slave = true;
            p_sys->i_next_demux_time = va_arg( args, int64_t ) - VLC_TS_0;
            return VLC_SUCCESS;

        case DEMUX_GET_PTS_DELAY:
        case DEMUX_GET_FPS:
        case DEMUX_GET_META:
        case DEMUX_GET_ATTACHMENTS:
        case DEMUX_GET_TITLE_INFO:
        case DEMUX_HAS_UNSUPPORTED_META:
        case DEMUX_CAN_RECORD:
        default:
            break;

    }
    return VLC_EGENERIC;
}

/*****************************************************************************
 * Demux: Send subtitle to decoder
 *****************************************************************************/
static int Demux( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    int64_t i_barrier = p_sys->i_next_demux_time;
    while( p_sys->cues.i_current < p_sys->cues.i_count &&
           p_sys->cues.p_array[p_sys->cues.i_current].i_start <= i_barrier )
    {
        const webvtt_cue_t *p_cue = &p_sys->cues.p_array[p_sys->cues.i_current];

        if ( !p_sys->b_slave && p_sys->b_first_time )
        {
            es_out_SetPCR( p_demux->out, VLC_TS_0 + i_barrier );
            p_sys->b_first_time = false;
        }

        if( p_cue->i_start >= 0 )
        {
            block_t *p_block = ConvertWEBVTT( p_cue, p_sys->cues.i_current > 0 );
            if( p_block )
            {
                p_block->i_dts =
                p_block->i_pts = VLC_TS_0 + p_cue->i_start;
                if( p_cue->i_stop >= 0 && p_cue->i_stop >= p_cue->i_start )
                    p_block->i_length = p_cue->i_stop - p_cue->i_start;

                if( p_sys->i_next_block_flags )
                {
                    p_block->i_flags = p_sys->i_next_block_flags;
                    p_sys->i_next_block_flags = 0;
                }
                es_out_Send( p_demux->out, p_sys->es, p_block );
            }
        }

        p_sys->cues.i_current++;
    }

    if ( !p_sys->b_slave )
    {
        es_out_SetPCR( p_demux->out, VLC_TS_0 + i_barrier );
        p_sys->i_next_demux_time += CLOCK_FREQ / 8;
    }

    if( p_sys->cues.i_current >= p_sys->cues.i_count )
        return VLC_DEMUXER_EOF;

    return VLC_DEMUXER_SUCCESS;
}


/*****************************************************************************
 * Module initializer
 *****************************************************************************/
int OpenDemux ( vlc_object_t *p_this )
{
    demux_t        *p_demux = (demux_t*)p_this;
    demux_sys_t    *p_sys;

    const uint8_t *p_peek;
    size_t i_peek = vlc_stream_Peek( p_demux->s, &p_peek, 16 );
    if( i_peek < 16 )
        return VLC_EGENERIC;

    if( !memcmp( p_peek, "\xEF\xBB\xBF", 3 ) )
        p_peek += 3;

    if( ( memcmp( p_peek, "WEBVTT", 6 ) ||
          ( p_peek[6] != '\n' &&
            p_peek[6] != ' ' &&
            p_peek[6] != '\t' &&
           ( p_peek[6] != '\r' || p_peek[7] != '\n' ) )
        ) && !p_demux->obj.force )
    {
        msg_Dbg( p_demux, "subtitle demux discarded" );
        return VLC_EGENERIC;
    }

    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;
    p_demux->p_sys = p_sys = malloc( sizeof( demux_sys_t ) );
    if( p_sys == NULL )
        return VLC_ENOMEM;

    p_sys->i_next_block_flags = 0;
    p_sys->i_next_demux_time = 0;
    p_sys->i_length = 0;
    p_sys->b_slave = false;

    p_sys->regions_headers.p_data = NULL;
    p_sys->regions_headers.i_data = 0;
    p_sys->styles_headers.p_data = NULL;
    p_sys->styles_headers.i_data = 0;

    p_sys->cues.i_count = 0;
    p_sys->cues.i_alloc = 0;
    p_sys->cues.p_array = NULL;
    p_sys->cues.i_current = 0;

    if( ReadWEBVTT( p_demux ) != VLC_SUCCESS )
    {
        CloseDemux( p_this );
        return VLC_EGENERIC;
    }

    es_format_t fmt;
    es_format_Init( &fmt, SPU_ES, VLC_CODEC_WEBVTT );
    size_t i_extra = 0;
    MakeExtradata( p_sys, &fmt.p_extra, &i_extra );
    fmt.i_extra = i_extra;
    p_sys->es = es_out_Add( p_demux->out, &fmt );
    es_format_Clean( &fmt );
    if( p_sys->es == NULL )
    {
        CloseDemux( p_this );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: Close subtitle demux
 *****************************************************************************/
void CloseDemux( vlc_object_t *p_this )
{
    demux_t *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;

    for( size_t i=0; i< p_sys->cues.i_count; i++ )
        webvtt_cue_Clean( &p_sys->cues.p_array[i] );
    free( p_sys->cues.p_array );

    free( p_sys );
}
