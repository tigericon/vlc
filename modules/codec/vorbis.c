/*****************************************************************************
 * vorbis.c: vorbis decoder/encoder/packetizer module making use of libvorbis.
 *****************************************************************************
 * Copyright (C) 2001-2003 VideoLAN
 * $Id: vorbis.c,v 1.30 2004/01/25 18:20:12 bigben Exp $
 *
 * Authors: Gildas Bazin <gbazin@netcourrier.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/decoder.h>
#include "vlc_playlist.h"

#include <ogg/ogg.h>

#ifdef MODULE_NAME_IS_tremor
#include <tremor/ivorbiscodec.h>

#else
#include <vorbis/codec.h>

/* vorbis header */
#ifdef HAVE_VORBIS_VORBISENC_H
#   include <vorbis/vorbisenc.h>
#   ifndef OV_ECTL_RATEMANAGE_AVG
#       define OV_ECTL_RATEMANAGE_AVG 0x0
#   endif
#endif

#endif

/*****************************************************************************
 * decoder_sys_t : vorbis decoder descriptor
 *****************************************************************************/
struct decoder_sys_t
{
    /* Module mode */
    vlc_bool_t b_packetizer;

    /*
     * Input properties
     */
    int i_headers;

    /*
     * Vorbis properties
     */
    vorbis_info      vi; /* struct that stores all the static vorbis bitstream
                            settings */
    vorbis_comment   vc; /* struct that stores all the bitstream user
                          * comments */
    vorbis_dsp_state vd; /* central working state for the packet->PCM
                          * decoder */
    vorbis_block     vb; /* local working space for packet->PCM decode */

    /*
     * Common properties
     */
    audio_date_t end_date;
    int          i_last_block_size;

};

static int pi_channels_maps[7] =
{
    0,
    AOUT_CHAN_CENTER,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
    AOUT_CHAN_CENTER | AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_REARLEFT
     | AOUT_CHAN_REARRIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
     | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT,
    AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
     | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT | AOUT_CHAN_LFE
};

/****************************************************************************
 * Local prototypes
 ****************************************************************************/
static int  OpenDecoder   ( vlc_object_t * );
static int  OpenPacketizer( vlc_object_t * );
static void CloseDecoder  ( vlc_object_t * );
static void *DecodeBlock  ( decoder_t *, block_t ** );

static void *ProcessPacket ( decoder_t *, ogg_packet *, block_t ** );

static aout_buffer_t *DecodePacket  ( decoder_t *, ogg_packet * );
static block_t *SendPacket( decoder_t *, ogg_packet *, block_t * );

static void ParseVorbisComments( decoder_t * );

#ifdef MODULE_NAME_IS_tremor
static void Interleave   ( int32_t *, const int32_t **, int, int );
#else
static void Interleave   ( float *, const float **, int, int );
#endif

#ifndef MODULE_NAME_IS_tremor
static int OpenEncoder   ( vlc_object_t * );
static void CloseEncoder ( vlc_object_t * );
static block_t *Headers  ( encoder_t * );
static block_t *Encode   ( encoder_t *, aout_buffer_t * );
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
vlc_module_begin();

    set_description( _("Vorbis audio decoder") );
#ifdef MODULE_NAME_IS_tremor
    set_capability( "decoder", 90 );
#else
    set_capability( "decoder", 100 );
#endif
    set_callbacks( OpenDecoder, CloseDecoder );

    add_submodule();
    set_description( _("Vorbis audio packetizer") );
    set_capability( "packetizer", 100 );
    set_callbacks( OpenPacketizer, CloseDecoder );

#ifndef MODULE_NAME_IS_tremor
    add_submodule();
    set_description( _("Vorbis audio encoder") );
    set_capability( "encoder", 100 );
    set_callbacks( OpenEncoder, CloseEncoder );
#endif

vlc_module_end();

/*****************************************************************************
 * OpenDecoder: probe the decoder and return score
 *****************************************************************************/
static int OpenDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;

    if( p_dec->fmt_in.i_codec != VLC_FOURCC('v','o','r','b') )
    {
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys =
          (decoder_sys_t *)malloc(sizeof(decoder_sys_t)) ) == NULL )
    {
        msg_Err( p_dec, "out of memory" );
        return VLC_EGENERIC;
    }

    /* Misc init */
    aout_DateSet( &p_sys->end_date, 0 );
    p_sys->b_packetizer = VLC_FALSE;
    p_sys->i_headers = 0;

    /* Take care of vorbis init */
    vorbis_info_init( &p_sys->vi );
    vorbis_comment_init( &p_sys->vc );

    /* Set output properties */
    p_dec->fmt_out.i_cat = AUDIO_ES;
#ifdef MODULE_NAME_IS_tremor
    p_dec->fmt_out.i_codec = VLC_FOURCC('f','i','3','2');
#else
    p_dec->fmt_out.i_codec = VLC_FOURCC('f','l','3','2');
#endif

    /* Set callbacks */
    p_dec->pf_decode_audio = (aout_buffer_t *(*)(decoder_t *, block_t **))
        DecodeBlock;
    p_dec->pf_packetize    = (block_t *(*)(decoder_t *, block_t **))
        DecodeBlock;

    return VLC_SUCCESS;
}

static int OpenPacketizer( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;

    int i_ret = OpenDecoder( p_this );

    if( i_ret == VLC_SUCCESS )
    {
        p_dec->p_sys->b_packetizer = VLC_TRUE;
        p_dec->fmt_out.i_codec = VLC_FOURCC('v','o','r','b');
    }

    return i_ret;
}

/****************************************************************************
 * DecodeBlock: the whole thing
 ****************************************************************************
 * This function must be fed with ogg packets.
 ****************************************************************************/
static void *DecodeBlock( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    ogg_packet oggpacket;

    if( !pp_block ) return NULL;

    if( *pp_block )
    {
        /* Block to Ogg packet */
        oggpacket.packet = (*pp_block)->p_buffer;
        oggpacket.bytes = (*pp_block)->i_buffer;
    }
    else
    {
        if( p_sys->b_packetizer ) return NULL;

        /* Block to Ogg packet */
        oggpacket.packet = NULL;
        oggpacket.bytes = 0;
    }

    oggpacket.granulepos = -1;
    oggpacket.b_o_s = 0;
    oggpacket.e_o_s = 0;
    oggpacket.packetno = 0;

    if( p_sys->i_headers == 0 )
    {
        /* Take care of the initial Vorbis header */

        oggpacket.b_o_s = 1; /* yes this actually is a b_o_s packet :) */
        if( vorbis_synthesis_headerin( &p_sys->vi, &p_sys->vc,
                                       &oggpacket ) < 0 )
        {
            msg_Err( p_dec, "this bitstream does not contain Vorbis "
                     "audio data.");
            block_Release( *pp_block );
            return NULL;
        }
        p_sys->i_headers++;

        /* Setup the format */
        p_dec->fmt_out.audio.i_rate     = p_sys->vi.rate;
        p_dec->fmt_out.audio.i_channels = p_sys->vi.channels;
        p_dec->fmt_out.audio.i_physical_channels =
            p_dec->fmt_out.audio.i_original_channels =
                pi_channels_maps[p_sys->vi.channels];
        p_dec->fmt_out.i_bitrate = p_sys->vi.bitrate_nominal;

        aout_DateInit( &p_sys->end_date, p_sys->vi.rate );

        msg_Dbg( p_dec, "channels:%d samplerate:%ld bitrate:%ld",
                 p_sys->vi.channels, p_sys->vi.rate,
                 p_sys->vi.bitrate_nominal );

        return ProcessPacket( p_dec, &oggpacket, pp_block );
    }

    if( p_sys->i_headers == 1 )
    {
        /* The next packet in order is the comments header */
        if( vorbis_synthesis_headerin( &p_sys->vi, &p_sys->vc, &oggpacket )
            < 0 )
        {
            msg_Err( p_dec, "2nd Vorbis header is corrupted" );
            block_Release( *pp_block );
            return NULL;
        }
        p_sys->i_headers++;

        ParseVorbisComments( p_dec );

        return ProcessPacket( p_dec, &oggpacket, pp_block );
    }

    if( p_sys->i_headers == 2 )
    {
        /* The next packet in order is the codebooks header
           We need to watch out that this packet is not missing as a
           missing or corrupted header is fatal. */
        if( vorbis_synthesis_headerin( &p_sys->vi, &p_sys->vc, &oggpacket )
            < 0 )
        {
            msg_Err( p_dec, "3rd Vorbis header is corrupted" );
            block_Release( *pp_block );
            return NULL;
        }
        p_sys->i_headers++;

        if( !p_sys->b_packetizer )
        {
            /* Initialize the Vorbis packet->PCM decoder */
            vorbis_synthesis_init( &p_sys->vd, &p_sys->vi );
            vorbis_block_init( &p_sys->vd, &p_sys->vb );
        }

        return ProcessPacket( p_dec, &oggpacket, pp_block );
    }

    return ProcessPacket( p_dec, &oggpacket, pp_block );
}

/*****************************************************************************
 * ProcessPacket: processes a Vorbis packet.
 *****************************************************************************/
static void *ProcessPacket( decoder_t *p_dec, ogg_packet *p_oggpacket,
                            block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_block = *pp_block;

    /* Date management */
    if( p_block && p_block->i_pts > 0 &&
        p_block->i_pts != aout_DateGet( &p_sys->end_date ) )
    {
        aout_DateSet( &p_sys->end_date, p_block->i_pts );
    }

    if( !aout_DateGet( &p_sys->end_date ) )
    {
        /* We've just started the stream, wait for the first PTS. */
        if( p_block ) block_Release( p_block );
        return NULL;
    }

    *pp_block = NULL; /* To avoid being fed the same packet again */

    if( p_sys->b_packetizer )
    {
        return SendPacket( p_dec, p_oggpacket, p_block );
    }
    else
    {
        aout_buffer_t *p_aout_buffer;

        if( p_sys->i_headers >= 3 )
            p_aout_buffer = DecodePacket( p_dec, p_oggpacket );
        else
            p_aout_buffer = NULL;

        if( p_block )
        {
            block_Release( p_block );
        }
        return p_aout_buffer;
    }
}

/*****************************************************************************
 * DecodePacket: decodes a Vorbis packet.
 *****************************************************************************/
static aout_buffer_t *DecodePacket( decoder_t *p_dec, ogg_packet *p_oggpacket )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int           i_samples;

#ifdef MODULE_NAME_IS_tremor
    int32_t       **pp_pcm;
#else
    float         **pp_pcm;
#endif

    if( p_oggpacket->bytes &&
        vorbis_synthesis( &p_sys->vb, p_oggpacket ) == 0 )
        vorbis_synthesis_blockin( &p_sys->vd, &p_sys->vb );

    /* **pp_pcm is a multichannel float vector. In stereo, for
     * example, pp_pcm[0] is left, and pp_pcm[1] is right. i_samples is
     * the size of each channel. Convert the float values
     * (-1.<=range<=1.) to whatever PCM format and write it out */

    if( ( i_samples = vorbis_synthesis_pcmout( &p_sys->vd, &pp_pcm ) ) > 0 )
    {

        aout_buffer_t *p_aout_buffer;

        p_aout_buffer =
            p_dec->pf_aout_buffer_new( p_dec, i_samples );

        if( p_aout_buffer == NULL ) return NULL;

        /* Interleave the samples */
#ifdef MODULE_NAME_IS_tremor
        Interleave( (int32_t *)p_aout_buffer->p_buffer,
                    (const int32_t **)pp_pcm, p_sys->vi.channels, i_samples );
#else
        Interleave( (float *)p_aout_buffer->p_buffer,
                    (const float **)pp_pcm, p_sys->vi.channels, i_samples );
#endif

        /* Tell libvorbis how many samples we actually consumed */
        vorbis_synthesis_read( &p_sys->vd, i_samples );

        /* Date management */
        p_aout_buffer->start_date = aout_DateGet( &p_sys->end_date );
        p_aout_buffer->end_date = aout_DateIncrement( &p_sys->end_date,
                                                      i_samples );
        return p_aout_buffer;
    }
    else
    {
        return NULL;
    }
}

/*****************************************************************************
 * SendPacket: send an ogg dated packet to the stream output.
 *****************************************************************************/
static block_t *SendPacket( decoder_t *p_dec, ogg_packet *p_oggpacket,
                            block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    int i_block_size, i_samples;

    i_block_size = vorbis_packet_blocksize( &p_sys->vi, p_oggpacket );
    if( i_block_size < 0 ) i_block_size = 0; /* non audio packet */
    i_samples = ( p_sys->i_last_block_size + i_block_size ) >> 2;
    p_sys->i_last_block_size = i_block_size;

    /* Date management */
    p_block->i_dts = p_block->i_pts = aout_DateGet( &p_sys->end_date );

    if( p_sys->i_headers >= 3 )
        p_block->i_length = aout_DateIncrement( &p_sys->end_date, i_samples ) -
            p_block->i_pts;
    else
        p_block->i_length = 0;

    return p_block;
}

/*****************************************************************************
 * ParseVorbisComments: FIXME should be done in demuxer
 *****************************************************************************/
static void ParseVorbisComments( decoder_t *p_dec )
{
    input_thread_t *p_input = (input_thread_t *)p_dec->p_parent;
    input_info_category_t *p_cat =
        input_InfoCategory( p_input, _("Vorbis comment") );
    playlist_t *p_playlist = vlc_object_find( p_dec, VLC_OBJECT_PLAYLIST,
                                              FIND_ANYWHERE );
    int i = 0;
    char *psz_name, *psz_value, *psz_comment;
    while ( i < p_dec->p_sys->vc.comments )
    {
        psz_comment = strdup( p_dec->p_sys->vc.user_comments[i] );
        if( !psz_comment )
        {
            msg_Warn( p_dec, "out of memory" );
            break;
        }
        psz_name = psz_comment;
        psz_value = strchr( psz_comment, '=' );
        if( psz_value )
        {
            *psz_value = '\0';
            psz_value++;
            input_AddInfo( p_cat, psz_name, psz_value );
            playlist_AddInfo( p_playlist, -1, _("Vorbis comment") ,
                              psz_name, psz_value );
        }
        free( psz_comment );
        i++;
    }
    if( p_playlist ) vlc_object_release( p_playlist );
}

/*****************************************************************************
 * Interleave: helper function to interleave channels
 *****************************************************************************/
static void Interleave(
#ifdef MODULE_NAME_IS_tremor
                        int32_t *p_out, const int32_t **pp_in,
#else
                        float *p_out, const float **pp_in,
#endif
                        int i_nb_channels, int i_samples )
{
    int i, j;

    for ( j = 0; j < i_samples; j++ )
    {
        for ( i = 0; i < i_nb_channels; i++ )
        {
            p_out[j * i_nb_channels + i] = pp_in[i][j];
        }
    }
}

/*****************************************************************************
 * CloseDecoder: vorbis decoder destruction
 *****************************************************************************/
static void CloseDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if( !p_sys->b_packetizer && p_sys->i_headers >= 3 )
    {
        vorbis_block_clear( &p_sys->vb );
        vorbis_dsp_clear( &p_sys->vd );
    }

    vorbis_comment_clear( &p_sys->vc );
    vorbis_info_clear( &p_sys->vi );  /* must be called last */

    free( p_sys );
}

#if defined(HAVE_VORBIS_VORBISENC_H) && !defined(MODULE_NAME_IS_tremor)

/*****************************************************************************
 * encoder_sys_t : theora encoder descriptor
 *****************************************************************************/
struct encoder_sys_t
{
    /*
     * Input properties
     */
    int i_headers;

    /*
     * Vorbis properties
     */
    vorbis_info      vi; /* struct that stores all the static vorbis bitstream
                            settings */
    vorbis_comment   vc; /* struct that stores all the bitstream user
                          * comments */
    vorbis_dsp_state vd; /* central working state for the packet->PCM
                          * decoder */
    vorbis_block     vb; /* local working space for packet->PCM decode */

    int i_last_block_size;
    int i_samples_delay;
    int i_channels;

    /*
     * Common properties
     */
    mtime_t i_pts;
};

/*****************************************************************************
 * OpenEncoder: probe the encoder and return score
 *****************************************************************************/
static int OpenEncoder( vlc_object_t *p_this )
{
    encoder_t *p_enc = (encoder_t *)p_this;
    encoder_sys_t *p_sys;

    if( p_enc->fmt_out.i_codec != VLC_FOURCC('v','o','r','b') )
    {
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_sys = (encoder_sys_t *)malloc(sizeof(encoder_sys_t)) ) == NULL )
    {
        msg_Err( p_enc, "out of memory" );
        return VLC_EGENERIC;
    }
    p_enc->p_sys = p_sys;

    p_enc->pf_header = Headers;
    p_enc->pf_encode_audio = Encode;
    p_enc->fmt_in.i_codec = VLC_FOURCC('f','l','3','2');

    /* Initialize vorbis encoder */
    vorbis_info_init( &p_sys->vi );

    if( vorbis_encode_setup_managed( &p_sys->vi,
            p_enc->fmt_in.audio.i_channels, p_enc->fmt_in.audio.i_rate,
            -1, p_enc->fmt_out.i_bitrate, -1 ) ||
        vorbis_encode_ctl( &p_sys->vi, OV_ECTL_RATEMANAGE_AVG, NULL ) ||
        vorbis_encode_setup_init( &p_sys->vi ) )
    {
        ;
    }

    /* add a comment */
    vorbis_comment_init( &p_sys->vc);
    vorbis_comment_add_tag( &p_sys->vc, "ENCODER", "VLC media player");

    /* set up the analysis state and auxiliary encoding storage */
    vorbis_analysis_init( &p_sys->vd, &p_sys->vi );
    vorbis_block_init( &p_sys->vd, &p_sys->vb );

    p_sys->i_channels = p_enc->fmt_in.audio.i_channels;
    p_sys->i_last_block_size = 0;
    p_sys->i_samples_delay = 0;
    p_sys->i_headers = 0;
    p_sys->i_pts = 0;

    return VLC_SUCCESS;
}

/****************************************************************************
 * Encode: the whole thing
 ****************************************************************************
 * This function spits out ogg packets.
 ****************************************************************************/
static block_t *Headers( encoder_t *p_enc )
{
    encoder_sys_t *p_sys = p_enc->p_sys;
    block_t *p_block, *p_chain = NULL;

    /* Create theora headers */
    if( !p_sys->i_headers )
    {
        ogg_packet header[3];
        int i;

        vorbis_analysis_headerout( &p_sys->vd, &p_sys->vc,
                                   &header[0], &header[1], &header[2]);
        for( i = 0; i < 3; i++ )
        {
            p_block = block_New( p_enc, header[i].bytes );
            memcpy( p_block->p_buffer, header[i].packet, header[i].bytes );

            p_block->i_dts = p_block->i_pts = p_block->i_length = 0;

            block_ChainAppend( &p_chain, p_block );
        }
        p_sys->i_headers = 3;
    }

    return p_chain;
}

/****************************************************************************
 * Encode: the whole thing
 ****************************************************************************
 * This function spits out ogg packets.
 ****************************************************************************/
static block_t *Encode( encoder_t *p_enc, aout_buffer_t *p_aout_buf )
{
    encoder_sys_t *p_sys = p_enc->p_sys;
    ogg_packet oggpacket;
    block_t *p_block, *p_chain = NULL;
    float **buffer;
    int i;
    unsigned int j;

    p_sys->i_pts = p_aout_buf->start_date -
                (mtime_t)1000000 * (mtime_t)p_sys->i_samples_delay /
                (mtime_t)p_enc->fmt_in.audio.i_rate;

    p_sys->i_samples_delay += p_aout_buf->i_nb_samples;

    buffer = vorbis_analysis_buffer( &p_sys->vd, p_aout_buf->i_nb_samples );

    /* convert samples to float and uninterleave */
    for( i = 0; i < p_sys->i_channels; i++ )
    {
        for( j = 0 ; j < p_aout_buf->i_nb_samples ; j++ )
        {
            buffer[i][j]= ((float)( ((int16_t *)p_aout_buf->p_buffer )
                                    [j * p_sys->i_channels + i ] )) / 32768.f;
        }
    }

    vorbis_analysis_wrote( &p_sys->vd, p_aout_buf->i_nb_samples );

    while( vorbis_analysis_blockout( &p_sys->vd, &p_sys->vb ) == 1 )
    {
        int i_samples;

        vorbis_analysis( &p_sys->vb, NULL );
        vorbis_bitrate_addblock( &p_sys->vb );

        while( vorbis_bitrate_flushpacket( &p_sys->vd, &oggpacket ) )
        {
            int i_block_size;
            p_block = block_New( p_enc, oggpacket.bytes );
            memcpy( p_block->p_buffer, oggpacket.packet, oggpacket.bytes );

            i_block_size = vorbis_packet_blocksize( &p_sys->vi, &oggpacket );

            if( i_block_size < 0 ) i_block_size = 0;
            i_samples = ( p_sys->i_last_block_size + i_block_size ) >> 2;
            p_sys->i_last_block_size = i_block_size;

            p_block->i_length = (mtime_t)1000000 *
                (mtime_t)i_samples / (mtime_t)p_enc->fmt_in.audio.i_rate;

            p_block->i_dts = p_block->i_pts = p_sys->i_pts;

            p_sys->i_samples_delay -= i_samples;

            /* Update pts */
            p_sys->i_pts += p_block->i_length;
            block_ChainAppend( &p_chain, p_block );
        }
    }

    return p_chain;
}

/*****************************************************************************
 * CloseEncoder: theora encoder destruction
 *****************************************************************************/
static void CloseEncoder( vlc_object_t *p_this )
{
    encoder_t *p_enc = (encoder_t *)p_this;
    encoder_sys_t *p_sys = p_enc->p_sys;

    vorbis_block_clear( &p_sys->vb );
    vorbis_dsp_clear( &p_sys->vd );
    vorbis_comment_clear( &p_sys->vc );
    vorbis_info_clear( &p_sys->vi );  /* must be called last */

    free( p_sys );
}

#endif /* HAVE_VORBIS_VORBISENC_H && !MODULE_NAME_IS_tremor */
