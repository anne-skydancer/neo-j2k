/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, Anne Skydancer (neo-j2k)
 *
 * neo-j2k public C API (draft, version 0.x: it may still change before 1.0).
 *
 * The API is OpenJPEG's (2.5.4) with every name moved into neo-j2k's namespace: opj_* -> nj2k_*,
 * OPJ_* -> NJ2K_*, JPWL_* -> NJ2K_JPWL_*, enum tags prefixed with NJ2K_. The types are the same
 * structures with the same members in the same order, so OpenJPEG code compiles against
 * <neoj2k/neoj2k_openjpeg.h>, which maps the OpenJPEG names onto these ones at compile time
 * (doc/api.md). The library exports nj2k_* only, so it never clashes with an OpenJPEG linked
 * into the same program (the test oracle, or the OpenJPEG encoder a host keeps meanwhile).
 *
 * Stage 0 implements the version, CPU check, parameter defaults and image functions; stage 1b the
 * streams, the decoder up to the decode (codec, handlers, settings, limits, the header read, the
 * reduce factor, component selection, the decoded dimensions, the codestream information) and the
 * statuses; stage 1c the decode; stage 6a the encoder, for the settings the Second Life viewer
 * uses (doc/api.md, "The encoder"). What the core does not implement fails cleanly (NULL or
 * NJ2K_FALSE, and NJ2K_E_UNSUPPORTED from nj2k_get_status).
 *
 * The declarations below are derived from OpenJPEG's openjpeg.h, whose notice follows.
 */
/*
* The copyright in this software is being made available under the 2-clauses
* BSD License, included below. This software may be subject to other third
* party and contributor rights, including patent rights, and no such rights
* are granted under this license.
*
* Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
* Copyright (c) 2002-2014, Professor Benoit Macq
* Copyright (c) 2001-2003, David Janssens
* Copyright (c) 2002-2003, Yannick Verschueren
* Copyright (c) 2003-2007, Francois-Olivier Devaux
* Copyright (c) 2003-2014, Antonin Descampe
* Copyright (c) 2005, Herve Drolon, FreeImage Team
* Copyright (c) 2006-2007, Parvatha Elangovan
* Copyright (c) 2008, Jerome Fimes, Communications & Systemes <jerome.fimes@c-s.fr>
* Copyright (c) 2010-2011, Kaori Hagihara
* Copyright (c) 2011-2012, Centre National d'Etudes Spatiales (CNES), France
* Copyright (c) 2012, CS Systemes d'Information, France
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
* 1. Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 2. Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the distribution.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
* AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
* IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
* ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
* INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
* CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
* ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
* POSSIBILITY OF SUCH DAMAGE.
*/
#ifndef NEOJ2K_H
#define NEOJ2K_H


/*
==========================================================
   Compiler directives
==========================================================
*/

/* deprecated attribute */
#ifdef __GNUC__
#define NJ2K_DEPRECATED(func) func __attribute__ ((deprecated))
#elif defined(_MSC_VER)
#define NJ2K_DEPRECATED(func) __declspec(deprecated) func
#else
#pragma message("WARNING: You need to implement DEPRECATED for this compiler")
#define NJ2K_DEPRECATED(func) func
#endif

#if defined(__GNUC__) && __GNUC__ >= 6
#define NJ2K_DEPRECATED_STRUCT_MEMBER(memb, msg) __attribute__ ((deprecated(msg))) memb
#else
#define NJ2K_DEPRECATED_STRUCT_MEMBER(memb, msg) memb
#endif

/*
 * Export and calling convention. Define NJ2K_STATIC when linking the static library; the build
 * defines NJ2K_BUILDING for the shared one. Windows: the DLL exports with __declspec(dllexport).
 * Elsewhere the shared library gives the API default visibility and the static library hidden
 * visibility, so a host that links it statically does not re-export it. x86-64 has one calling
 * convention: NJ2K_CALLCONV is empty and stands for OpenJPEG's OPJ_CALLCONV.
 */
#if defined(_WIN32)
#   if defined(NJ2K_STATIC)
#       define NJ2K_API
#   elif defined(NJ2K_BUILDING)
#       define NJ2K_API __declspec(dllexport)
#   else
#       define NJ2K_API __declspec(dllimport)
#   endif
#   define NJ2K_LOCAL
#elif defined(__GNUC__) && __GNUC__ >= 4
#   if defined(NJ2K_STATIC)
#       define NJ2K_API __attribute__ ((visibility ("hidden")))
#   else
#       define NJ2K_API __attribute__ ((visibility ("default")))
#   endif
#   define NJ2K_LOCAL __attribute__ ((visibility ("hidden")))
#else
#   define NJ2K_API
#   define NJ2K_LOCAL
#endif
#define NJ2K_CALLCONV

typedef int NJ2K_BOOL;
#define NJ2K_TRUE 1
#define NJ2K_FALSE 0

typedef char          NJ2K_CHAR;
typedef float         NJ2K_FLOAT32;
typedef double        NJ2K_FLOAT64;
typedef unsigned char NJ2K_BYTE;

#include <stdint.h>

typedef int8_t   NJ2K_INT8;
typedef uint8_t  NJ2K_UINT8;
typedef int16_t  NJ2K_INT16;
typedef uint16_t NJ2K_UINT16;
typedef int32_t  NJ2K_INT32;
typedef uint32_t NJ2K_UINT32;
typedef int64_t  NJ2K_INT64;
typedef uint64_t NJ2K_UINT64;

typedef int64_t  NJ2K_OFF_T; /* 64-bit file offset type */

#include <stdio.h>
typedef size_t   NJ2K_SIZE_T;

/*
 * neo-j2k's own version (SemVer; the ABI generation is the major version once 1.0 freezes it).
 * The compatibility header defines OPJ_VERSION_MAJOR/MINOR/BUILD as the OpenJPEG API level this
 * API mirrors (2.5.4), not as neo-j2k's version.
 */
#define NJ2K_VERSION_MAJOR 0
#define NJ2K_VERSION_MINOR 1
#define NJ2K_VERSION_PATCH 3

/* Avoid compile-time warning because parameter is not used */
#define NJ2K_ARG_NOT_USED(x) (void)(x)

/*
==========================================================
   Useful constant definitions
==========================================================
*/

#define NJ2K_PATH_LEN 4096 /**< Maximum allowed size for filenames */

#define NJ2K_J2K_MAXRLVLS 33                 /**< Number of maximum resolution level authorized */
#define NJ2K_J2K_MAXBANDS (3*NJ2K_J2K_MAXRLVLS-2) /**< Number of maximum sub-band linked to number of resolution level */

#define NJ2K_J2K_DEFAULT_NB_SEGS             10
#define NJ2K_J2K_STREAM_CHUNK_SIZE           0x100000 /** 1 mega by default */
#define NJ2K_J2K_DEFAULT_HEADER_SIZE         1000
#define NJ2K_J2K_MCC_DEFAULT_NB_RECORDS      10
#define NJ2K_J2K_MCT_DEFAULT_NB_RECORDS      10

/* UniPG>> */ /* NOT YET USED IN THE V2 VERSION OF OPENJPEG */
#define NJ2K_JPWL_MAX_NO_TILESPECS   16 /**< Maximum number of tile parts expected by JPWL: increase at your will */
#define NJ2K_JPWL_MAX_NO_PACKSPECS   16 /**< Maximum number of packet parts expected by JPWL: increase at your will */
#define NJ2K_JPWL_MAX_NO_MARKERS 512 /**< Maximum number of JPWL markers: increase at your will */
#define NJ2K_JPWL_PRIVATEINDEX_NAME "jpwl_index_privatefilename" /**< index file name used when JPWL is on */
#define NJ2K_JPWL_EXPECTED_COMPONENTS 3 /**< Expect this number of components, so you'll find better the first EPB */
#define NJ2K_JPWL_MAXIMUM_TILES 8192 /**< Expect this maximum number of tiles, to avoid some crashes */
#define NJ2K_JPWL_MAXIMUM_HAMMING 2 /**< Expect this maximum number of bit errors in marker id's */
#define NJ2K_JPWL_MAXIMUM_EPB_ROOM 65450 /**< Expect this maximum number of bytes for composition of EPBs */
/* <<UniPG */

/**
 * EXPERIMENTAL FOR THE MOMENT
 * Supported options about file information used only in j2k_dump
*/
#define NJ2K_IMG_INFO        1   /**< Basic image information provided to the user */
#define NJ2K_J2K_MH_INFO     2   /**< Codestream information based only on the main header */
#define NJ2K_J2K_TH_INFO     4   /**< Tile information based on the current tile header */
#define NJ2K_J2K_TCH_INFO    8   /**< Tile/Component information of all tiles */
#define NJ2K_J2K_MH_IND      16  /**< Codestream index based only on the main header */
#define NJ2K_J2K_TH_IND      32  /**< Tile index based on the current tile */
/*FIXME #define NJ2K_J2K_CSTR_IND    48*/    /**<  */
#define NJ2K_JP2_INFO        128 /**< JP2 file information */
#define NJ2K_JP2_IND         256 /**< JP2 file index */

/**
 * JPEG 2000 Profiles, see Table A.10 from 15444-1 (updated in various AMD)
 * These values help choosing the RSIZ value for the J2K codestream.
 * The RSIZ value triggers various encoding options, as detailed in Table A.10.
 * If NJ2K_PROFILE_PART2 is chosen, it has to be combined with one or more extensions
 * described hereunder.
 *   Example: rsiz = NJ2K_PROFILE_PART2 | NJ2K_EXTENSION_MCT;
 * For broadcast profiles, the NJ2K_PROFILE value has to be combined with the targeted
 * mainlevel (3-0 LSB, value between 0 and 11):
 *   Example: rsiz = NJ2K_PROFILE_BC_MULTI | 0x0005; (here mainlevel 5)
 * For IMF profiles, the NJ2K_PROFILE value has to be combined with the targeted mainlevel
 * (3-0 LSB, value between 0 and 11) and sublevel (7-4 LSB, value between 0 and 9):
 *   Example: rsiz = NJ2K_PROFILE_IMF_2K | 0x0040 | 0x0005; (here main 5 and sublevel 4)
 * */
#define NJ2K_PROFILE_NONE        0x0000 /** no profile, conform to 15444-1 */
#define NJ2K_PROFILE_0           0x0001 /** Profile 0 as described in 15444-1,Table A.45 */
#define NJ2K_PROFILE_1           0x0002 /** Profile 1 as described in 15444-1,Table A.45 */
#define NJ2K_PROFILE_PART2       0x8000 /** At least 1 extension defined in 15444-2 (Part-2) */
#define NJ2K_PROFILE_CINEMA_2K   0x0003 /** 2K cinema profile defined in 15444-1 AMD1 */
#define NJ2K_PROFILE_CINEMA_4K   0x0004 /** 4K cinema profile defined in 15444-1 AMD1 */
#define NJ2K_PROFILE_CINEMA_S2K  0x0005 /** Scalable 2K cinema profile defined in 15444-1 AMD2 */
#define NJ2K_PROFILE_CINEMA_S4K  0x0006 /** Scalable 4K cinema profile defined in 15444-1 AMD2 */
#define NJ2K_PROFILE_CINEMA_LTS  0x0007 /** Long term storage cinema profile defined in 15444-1 AMD2 */
#define NJ2K_PROFILE_BC_SINGLE   0x0100 /** Single Tile Broadcast profile defined in 15444-1 AMD3 */
#define NJ2K_PROFILE_BC_MULTI    0x0200 /** Multi Tile Broadcast profile defined in 15444-1 AMD3 */
#define NJ2K_PROFILE_BC_MULTI_R  0x0300 /** Multi Tile Reversible Broadcast profile defined in 15444-1 AMD3 */
#define NJ2K_PROFILE_IMF_2K      0x0400 /** 2K Single Tile Lossy IMF profile defined in 15444-1 AMD 8 */
#define NJ2K_PROFILE_IMF_4K      0x0500 /** 4K Single Tile Lossy IMF profile defined in 15444-1 AMD 8 */
#define NJ2K_PROFILE_IMF_8K      0x0600 /** 8K Single Tile Lossy IMF profile defined in 15444-1 AMD 8 */
#define NJ2K_PROFILE_IMF_2K_R    0x0700 /** 2K Single/Multi Tile Reversible IMF profile defined in 15444-1 AMD 8 */
#define NJ2K_PROFILE_IMF_4K_R    0x0800 /** 4K Single/Multi Tile Reversible IMF profile defined in 15444-1 AMD 8 */
#define NJ2K_PROFILE_IMF_8K_R    0x0900 /** 8K Single/Multi Tile Reversible IMF profile defined in 15444-1 AMD 8 */

/**
 * JPEG 2000 Part-2 extensions
 * */
#define NJ2K_EXTENSION_NONE      0x0000 /** No Part-2 extension */
#define NJ2K_EXTENSION_MCT       0x0100  /** Custom MCT support */

/**
 * JPEG 2000 profile macros
 * */
#define NJ2K_IS_CINEMA(v)     (((v) >= NJ2K_PROFILE_CINEMA_2K)&&((v) <= NJ2K_PROFILE_CINEMA_S4K))
#define NJ2K_IS_STORAGE(v)    ((v) == NJ2K_PROFILE_CINEMA_LTS)
#define NJ2K_IS_BROADCAST(v)  (((v) >= NJ2K_PROFILE_BC_SINGLE)&&((v) <= ((NJ2K_PROFILE_BC_MULTI_R) | (0x000b))))
#define NJ2K_IS_IMF(v)        (((v) >= NJ2K_PROFILE_IMF_2K)&&((v) <= ((NJ2K_PROFILE_IMF_8K_R) | (0x009b))))
#define NJ2K_IS_PART2(v)      ((v) & NJ2K_PROFILE_PART2)

#define NJ2K_GET_IMF_PROFILE(v)   ((v) & 0xff00)      /** Extract IMF profile without mainlevel/sublevel */
#define NJ2K_GET_IMF_MAINLEVEL(v) ((v) & 0xf)         /** Extract IMF main level */
#define NJ2K_GET_IMF_SUBLEVEL(v)  (((v) >> 4) & 0xf)  /** Extract IMF sub level */

#define NJ2K_IMF_MAINLEVEL_MAX    11   /** Maximum main level */

/** Max. Components Sampling Rate (MSamples/sec) per IMF main level */
#define NJ2K_IMF_MAINLEVEL_1_MSAMPLESEC   65      /** MSamples/sec for IMF main level 1 */
#define NJ2K_IMF_MAINLEVEL_2_MSAMPLESEC   130     /** MSamples/sec for IMF main level 2 */
#define NJ2K_IMF_MAINLEVEL_3_MSAMPLESEC   195     /** MSamples/sec for IMF main level 3 */
#define NJ2K_IMF_MAINLEVEL_4_MSAMPLESEC   260     /** MSamples/sec for IMF main level 4 */
#define NJ2K_IMF_MAINLEVEL_5_MSAMPLESEC   520     /** MSamples/sec for IMF main level 5 */
#define NJ2K_IMF_MAINLEVEL_6_MSAMPLESEC   1200    /** MSamples/sec for IMF main level 6 */
#define NJ2K_IMF_MAINLEVEL_7_MSAMPLESEC   2400    /** MSamples/sec for IMF main level 7 */
#define NJ2K_IMF_MAINLEVEL_8_MSAMPLESEC   4800    /** MSamples/sec for IMF main level 8 */
#define NJ2K_IMF_MAINLEVEL_9_MSAMPLESEC   9600    /** MSamples/sec for IMF main level 9 */
#define NJ2K_IMF_MAINLEVEL_10_MSAMPLESEC  19200   /** MSamples/sec for IMF main level 10 */
#define NJ2K_IMF_MAINLEVEL_11_MSAMPLESEC  38400   /** MSamples/sec for IMF main level 11 */

/** Max. compressed Bit Rate (Mbits/s) per IMF sub level */
#define NJ2K_IMF_SUBLEVEL_1_MBITSSEC      200     /** Mbits/s for IMF sub level 1 */
#define NJ2K_IMF_SUBLEVEL_2_MBITSSEC      400     /** Mbits/s for IMF sub level 2 */
#define NJ2K_IMF_SUBLEVEL_3_MBITSSEC      800     /** Mbits/s for IMF sub level 3 */
#define NJ2K_IMF_SUBLEVEL_4_MBITSSEC     1600     /** Mbits/s for IMF sub level 4 */
#define NJ2K_IMF_SUBLEVEL_5_MBITSSEC     3200     /** Mbits/s for IMF sub level 5 */
#define NJ2K_IMF_SUBLEVEL_6_MBITSSEC     6400     /** Mbits/s for IMF sub level 6 */
#define NJ2K_IMF_SUBLEVEL_7_MBITSSEC    12800     /** Mbits/s for IMF sub level 7 */
#define NJ2K_IMF_SUBLEVEL_8_MBITSSEC    25600     /** Mbits/s for IMF sub level 8 */
#define NJ2K_IMF_SUBLEVEL_9_MBITSSEC    51200     /** Mbits/s for IMF sub level 9 */

/**
 * JPEG 2000 codestream and component size limits in cinema profiles
 * */
#define NJ2K_CINEMA_24_CS     1302083    /** Maximum codestream length for 24fps */
#define NJ2K_CINEMA_48_CS     651041     /** Maximum codestream length for 48fps */
#define NJ2K_CINEMA_24_COMP   1041666    /** Maximum size per color component for 2K & 4K @ 24fps */
#define NJ2K_CINEMA_48_COMP   520833     /** Maximum size per color component for 2K @ 48fps */

/*
==========================================================
   enum definitions
==========================================================
*/

/**
 * DEPRECATED: use RSIZ, NJ2K_PROFILE_* and NJ2K_EXTENSION_* instead
 * Rsiz Capabilities
 * */
typedef enum NJ2K_RSIZ_CAPABILITIES {
    NJ2K_STD_RSIZ = 0,       /** Standard JPEG2000 profile*/
    NJ2K_CINEMA2K = 3,       /** Profile name for a 2K image*/
    NJ2K_CINEMA4K = 4,       /** Profile name for a 4K image*/
    NJ2K_MCT = 0x8100
} NJ2K_RSIZ_CAPABILITIES;

/**
 * DEPRECATED: use RSIZ, NJ2K_PROFILE_* and NJ2K_EXTENSION_* instead
 * Digital cinema operation mode
 * */
typedef enum NJ2K_CINEMA_MODE {
    NJ2K_OFF = 0,            /** Not Digital Cinema*/
    NJ2K_CINEMA2K_24 = 1,    /** 2K Digital Cinema at 24 fps*/
    NJ2K_CINEMA2K_48 = 2,    /** 2K Digital Cinema at 48 fps*/
    NJ2K_CINEMA4K_24 = 3     /** 4K Digital Cinema at 24 fps*/
} NJ2K_CINEMA_MODE;

/**
 * Progression order
 * */
typedef enum NJ2K_PROG_ORDER {
    NJ2K_PROG_UNKNOWN = -1,  /**< place-holder */
    NJ2K_LRCP = 0,           /**< layer-resolution-component-precinct order */
    NJ2K_RLCP = 1,           /**< resolution-layer-component-precinct order */
    NJ2K_RPCL = 2,           /**< resolution-precinct-component-layer order */
    NJ2K_PCRL = 3,           /**< precinct-component-resolution-layer order */
    NJ2K_CPRL = 4            /**< component-precinct-resolution-layer order */
} NJ2K_PROG_ORDER;

/**
 * Supported image color spaces
*/
typedef enum NJ2K_COLOR_SPACE {
    NJ2K_CLRSPC_UNKNOWN = -1,    /**< not supported by the library */
    NJ2K_CLRSPC_UNSPECIFIED = 0, /**< not specified in the codestream */
    NJ2K_CLRSPC_SRGB = 1,        /**< sRGB */
    NJ2K_CLRSPC_GRAY = 2,        /**< grayscale */
    NJ2K_CLRSPC_SYCC = 3,        /**< YUV */
    NJ2K_CLRSPC_EYCC = 4,        /**< e-YCC */
    NJ2K_CLRSPC_CMYK = 5         /**< CMYK */
} NJ2K_COLOR_SPACE;

/**
 * Supported codec
*/
typedef enum NJ2K_CODEC_FORMAT {
    NJ2K_CODEC_UNKNOWN = -1, /**< place-holder */
    NJ2K_CODEC_J2K  = 0,     /**< JPEG-2000 codestream : read/write */
    NJ2K_CODEC_JPT  = 1,     /**< JPT-stream (JPEG 2000, JPIP) : read only */
    NJ2K_CODEC_JP2  = 2,     /**< JP2 file format : read/write */
    NJ2K_CODEC_JPP  = 3,     /**< JPP-stream (JPEG 2000, JPIP) : to be coded */
    NJ2K_CODEC_JPX  = 4      /**< JPX file format (JPEG 2000 Part-2) : to be coded */
} NJ2K_CODEC_FORMAT;


/*
==========================================================
   event manager typedef definitions
==========================================================
*/

/**
 * Callback function prototype for events
 * @param msg               Event message
 * @param client_data       Client object where will be return the event message
 * */
typedef void (*nj2k_msg_callback)(const char *msg, void *client_data);

/*
==========================================================
   codec typedef definitions
==========================================================
*/

#ifndef NJ2K_UINT32_SEMANTICALLY_BUT_INT32
#define NJ2K_UINT32_SEMANTICALLY_BUT_INT32 NJ2K_INT32
#endif

/**
 * Progression order changes
 *
 */
typedef struct nj2k_poc {
    /** Resolution num start, Component num start, given by POC */
    NJ2K_UINT32 resno0, compno0;
    /** Layer num end,Resolution num end, Component num end, given by POC */
    NJ2K_UINT32 layno1, resno1, compno1;
    /** Layer num start,Precinct num start, Precinct num end */
    NJ2K_UINT32 layno0, precno0, precno1;
    /** Progression order enum*/
    NJ2K_PROG_ORDER prg1, prg;
    /** Progression order string*/
    NJ2K_CHAR progorder[5];
    /** Tile number (starting at 1) */
    NJ2K_UINT32 tile;
    /** Start and end values for Tile width and height*/
    NJ2K_UINT32_SEMANTICALLY_BUT_INT32 tx0, tx1, ty0, ty1;
    /** Start value, initialised in pi_initialise_encode*/
    NJ2K_UINT32 layS, resS, compS, prcS;
    /** End value, initialised in pi_initialise_encode */
    NJ2K_UINT32 layE, resE, compE, prcE;
    /** Start and end values of Tile width and height, initialised in pi_initialise_encode*/
    NJ2K_UINT32 txS, txE, tyS, tyE, dx, dy;
    /** Temporary values for Tile parts, initialised in pi_create_encode */
    NJ2K_UINT32 lay_t, res_t, comp_t, prc_t, tx0_t, ty0_t;
} nj2k_poc_t;

/**
 * Compression parameters
 * */
typedef struct nj2k_cparameters {
    /** size of tile: tile_size_on = false (not in argument) or = true (in argument) */
    NJ2K_BOOL tile_size_on;
    /** XTOsiz */
    int cp_tx0;
    /** YTOsiz */
    int cp_ty0;
    /** XTsiz */
    int cp_tdx;
    /** YTsiz */
    int cp_tdy;
    /** allocation by rate/distortion */
    int cp_disto_alloc;
    /** allocation by fixed layer */
    int cp_fixed_alloc;
    /** allocation by fixed quality (PSNR) */
    int cp_fixed_quality;
    /** fixed layer */
    int *cp_matrice;
    /** comment for coding */
    char *cp_comment;
    /** csty : coding style */
    int csty;
    /** progression order (default NJ2K_LRCP) */
    NJ2K_PROG_ORDER prog_order;
    /** progression order changes */
    nj2k_poc_t POC[32];
    /** number of progression order changes (POC), default to 0 */
    NJ2K_UINT32 numpocs;
    /** number of layers */
    int tcp_numlayers;
    /** rates of layers - might be subsequently limited by the max_cs_size field.
     * Should be decreasing. 1 can be
     * used as last value to indicate the last layer is lossless. */
    float tcp_rates[100];
    /** different psnr for successive layers. Should be increasing. 0 can be
     * used as last value to indicate the last layer is lossless. */
    float tcp_distoratio[100];
    /** number of resolutions */
    int numresolution;
    /** initial code block width, default to 64 */
    int cblockw_init;
    /** initial code block height, default to 64 */
    int cblockh_init;
    /** mode switch (cblk_style) */
    int mode;
    /** 1 : use the irreversible DWT 9-7, 0 : use lossless compression (default) */
    int irreversible;
    /** region of interest: affected component in [0..3], -1 means no ROI */
    int roi_compno;
    /** region of interest: upshift value */
    int roi_shift;
    /* number of precinct size specifications */
    int res_spec;
    /** initial precinct width */
    int prcw_init[NJ2K_J2K_MAXRLVLS];
    /** initial precinct height */
    int prch_init[NJ2K_J2K_MAXRLVLS];

    /**@name command line encoder parameters (not used inside the library) */
    /*@{*/
    /** input file name */
    char infile[NJ2K_PATH_LEN];
    /** output file name */
    char outfile[NJ2K_PATH_LEN];
    /** DEPRECATED. Index generation is now handled with the nj2k_encode_with_info() function. Set to NULL */
    int index_on;
    /** DEPRECATED. Index generation is now handled with the nj2k_encode_with_info() function. Set to NULL */
    char index[NJ2K_PATH_LEN];
    /** subimage encoding: origin image offset in x direction */
    int image_offset_x0;
    /** subimage encoding: origin image offset in y direction */
    int image_offset_y0;
    /** subsampling value for dx */
    int subsampling_dx;
    /** subsampling value for dy */
    int subsampling_dy;
    /** input file format 0: PGX, 1: PxM, 2: BMP 3:TIF*/
    int decod_format;
    /** output file format 0: J2K, 1: JP2, 2: JPT */
    int cod_format;
    /*@}*/

    /* UniPG>> */ /* NOT YET USED IN THE V2 VERSION OF OPENJPEG */
    /**@name JPWL encoding parameters */
    /*@{*/
    /** enables writing of EPC in MH, thus activating JPWL */
    NJ2K_BOOL jpwl_epc_on;
    /** error protection method for MH (0,1,16,32,37-128) */
    int jpwl_hprot_MH;
    /** tile number of header protection specification (>=0) */
    int jpwl_hprot_TPH_tileno[NJ2K_JPWL_MAX_NO_TILESPECS];
    /** error protection methods for TPHs (0,1,16,32,37-128) */
    int jpwl_hprot_TPH[NJ2K_JPWL_MAX_NO_TILESPECS];
    /** tile number of packet protection specification (>=0) */
    int jpwl_pprot_tileno[NJ2K_JPWL_MAX_NO_PACKSPECS];
    /** packet number of packet protection specification (>=0) */
    int jpwl_pprot_packno[NJ2K_JPWL_MAX_NO_PACKSPECS];
    /** error protection methods for packets (0,1,16,32,37-128) */
    int jpwl_pprot[NJ2K_JPWL_MAX_NO_PACKSPECS];
    /** enables writing of ESD, (0=no/1/2 bytes) */
    int jpwl_sens_size;
    /** sensitivity addressing size (0=auto/2/4 bytes) */
    int jpwl_sens_addr;
    /** sensitivity range (0-3) */
    int jpwl_sens_range;
    /** sensitivity method for MH (-1=no,0-7) */
    int jpwl_sens_MH;
    /** tile number of sensitivity specification (>=0) */
    int jpwl_sens_TPH_tileno[NJ2K_JPWL_MAX_NO_TILESPECS];
    /** sensitivity methods for TPHs (-1=no,0-7) */
    int jpwl_sens_TPH[NJ2K_JPWL_MAX_NO_TILESPECS];
    /*@}*/
    /* <<UniPG */

    /**
     * DEPRECATED: use RSIZ, NJ2K_PROFILE_* and MAX_COMP_SIZE instead
     * Digital Cinema compliance 0-not compliant, 1-compliant
     * */
    NJ2K_CINEMA_MODE cp_cinema;
    /**
     * Maximum size (in bytes) for each component.
     * If == 0, component size limitation is not considered
     * */
    int max_comp_size;
    /**
     * DEPRECATED: use RSIZ, NJ2K_PROFILE_* and NJ2K_EXTENSION_* instead
     * Profile name
     * */
    NJ2K_RSIZ_CAPABILITIES cp_rsiz;
    /** Tile part generation*/
    char tp_on;
    /** Flag for Tile part generation*/
    char tp_flag;
    /** MCT (multiple component transform) */
    char tcp_mct;
    /** Enable JPIP indexing*/
    NJ2K_BOOL jpip_on;
    /** Naive implementation of MCT restricted to a single reversible array based
        encoding without offset concerning all the components. */
    void * mct_data;
    /**
     * Maximum size (in bytes) for the whole codestream.
     * If == 0, codestream size limitation is not considered
     * If it does not comply with tcp_rates, max_cs_size prevails
     * and a warning is issued.
     * */
    int max_cs_size;
    /** RSIZ value
        To be used to combine NJ2K_PROFILE_*, NJ2K_EXTENSION_* and (sub)levels values. */
    NJ2K_UINT16 rsiz;
} nj2k_cparameters_t;

#define NJ2K_DPARAMETERS_IGNORE_PCLR_CMAP_CDEF_FLAG  0x0001
#define NJ2K_DPARAMETERS_DUMP_FLAG                   0x0002

/**
 * Decompression parameters
 * */
typedef struct nj2k_dparameters {
    /**
    Set the number of highest resolution levels to be discarded.
    The image resolution is effectively divided by 2 to the power of the number of discarded levels.
    The reduce factor is limited by the smallest total number of decomposition levels among tiles.
    if != 0, then original dimension divided by 2^(reduce);
    if == 0 or not used, image is decoded to the full resolution
    */
    NJ2K_UINT32 cp_reduce;
    /**
    Set the maximum number of quality layers to decode.
    If there are less quality layers than the specified number, all the quality layers are decoded.
    if != 0, then only the first "layer" layers are decoded;
    if == 0 or not used, all the quality layers are decoded
    */
    NJ2K_UINT32 cp_layer;

    /**@name command line decoder parameters (not used inside the library) */
    /*@{*/
    /** input file name */
    char infile[NJ2K_PATH_LEN];
    /** output file name */
    char outfile[NJ2K_PATH_LEN];
    /** input file format 0: J2K, 1: JP2, 2: JPT */
    int decod_format;
    /** output file format 0: PGX, 1: PxM, 2: BMP */
    int cod_format;

    /** Decoding area left boundary */
    NJ2K_UINT32 DA_x0;
    /** Decoding area right boundary */
    NJ2K_UINT32 DA_x1;
    /** Decoding area up boundary */
    NJ2K_UINT32 DA_y0;
    /** Decoding area bottom boundary */
    NJ2K_UINT32 DA_y1;
    /** Verbose mode */
    NJ2K_BOOL m_verbose;

    /** tile number of the decoded tile */
    NJ2K_UINT32 tile_index;
    /** Nb of tile to decode */
    NJ2K_UINT32 nb_tile_to_decode;

    /*@}*/

    /* UniPG>> */ /* NOT YET USED IN THE V2 VERSION OF OPENJPEG */
    /**@name JPWL decoding parameters */
    /*@{*/
    /** activates the JPWL correction capabilities */
    NJ2K_BOOL jpwl_correct;
    /** expected number of components */
    int jpwl_exp_comps;
    /** maximum number of tiles */
    int jpwl_max_tiles;
    /*@}*/
    /* <<UniPG */

    unsigned int flags;

} nj2k_dparameters_t;

/**
 * Defined by builds that have nj2k_decoder_limits_init() and nj2k_decoder_set_limits() (the
 * decoder limits API that the OpenJPEG fork's stage 1a hotfix adds to OpenJPEG 2.5.4).
 */
#define NJ2K_HAVE_DECODER_LIMITS 1

/**
 * Resource limits of a decoder, set with nj2k_decoder_set_limits().
 *
 * The decoder checks each limit from the codestream's headers before it allocates what the
 * limit bounds, and refuses a codestream that exceeds one: nj2k_read_header() or nj2k_decode()
 * then fails with an error message, and nj2k_get_status() gives NJ2K_E_LIMIT. A limit of 0
 * means no limit, and a decoder that is given no limits has none. Fill the structure with
 * nj2k_decoder_limits_init() and change single fields afterwards if needed.
 * */
typedef struct nj2k_decoder_limits {
    /** Tiles of the image (SIZ marker), checked when the main header is read. */
    NJ2K_UINT32 max_tiles;
    /** Components of the image (SIZ marker), checked when the main header is read. */
    NJ2K_UINT32 max_components;
    /** Quality layers (COD marker of the main header and of each tile), checked as each
     * COD marker is read. */
    NJ2K_UINT32 max_layers;
    /** Width and height of every component at the decoded resolution, after the reduce
     * factor, checked when decoding starts (and by nj2k_get_decoded_dimensions()). */
    NJ2K_UINT32 max_decoded_side;
    /** Bytes the decoder may allocate, checked against needs computed from the headers before
     * what they bound is allocated: the header's own state, as it is read; the working memory of
     * a decode (the decoded samples, the wavelet's lines, and the code-block, precinct and packet
     * state of every resolution of the largest tile with the main header's coding), when decoding
     * starts and by nj2k_get_decoded_dimensions(); and the working memory with a tile's own
     * coding, before a tile whose COD or COC changes it is decoded. While decoding, the
     * structures the decode holds and a tile's decoded samples share the limit. The codestream's
     * bytes are not counted: nj2k_decode() holds the stream's (up to the length the host gave, or
     * the end of a JP2 file's codestream box), and the memory API reads the caller's. */
    NJ2K_UINT64 max_memory;
} nj2k_decoder_limits_t;

/** No limits: the default of every decoder. */
#define NJ2K_DECODER_LIMITS_NONE        0
/**
 * Second Life textures: 1,024 tiles, 8 components, 64 quality layers, 2,048 pixels per
 * side at the decoded resolution and 256 MiB of working memory.
 */
#define NJ2K_DECODER_LIMITS_SL_TEXTURE  1


/**
 * JPEG2000 codec V2.
 * */
typedef void * nj2k_codec_t;

/*
==========================================================
   I/O stream typedef definitions
==========================================================
*/

/**
 * Stream open flags.
 * */
/** The stream was opened for reading. */
#define NJ2K_STREAM_READ NJ2K_TRUE
/** The stream was opened for writing. */
#define NJ2K_STREAM_WRITE NJ2K_FALSE

/*
 * Callback function prototype for read function
 * @return returns The number of bytes delivered into
 * \a p_buffer. -1 signals end of stream.
 */
typedef NJ2K_SIZE_T(* nj2k_stream_read_fn)(void * p_buffer, NJ2K_SIZE_T p_nb_bytes,
        void * p_user_data) ;

/*
 * Callback function prototype for write function
 */
typedef NJ2K_SIZE_T(* nj2k_stream_write_fn)(void * p_buffer,
        NJ2K_SIZE_T p_nb_bytes, void * p_user_data) ;

/*
 * Callback function prototype for skip function
 */
typedef NJ2K_OFF_T(* nj2k_stream_skip_fn)(NJ2K_OFF_T p_nb_bytes,
                                        void * p_user_data) ;

/*
 * Callback function prototype for seek function
 */
typedef NJ2K_BOOL(* nj2k_stream_seek_fn)(NJ2K_OFF_T p_nb_bytes,
                                       void * p_user_data) ;

/*
 * Callback function prototype for free user data function
 */
typedef void (* nj2k_stream_free_user_data_fn)(void * p_user_data) ;

/*
 * JPEG2000 Stream.
 */
typedef void * nj2k_stream_t;

/*
==========================================================
   image typedef definitions
==========================================================
*/

/**
 * Defines a single image component
 * */
typedef struct nj2k_image_comp {
    /** XRsiz: horizontal separation of a sample of ith component with respect to the reference grid */
    NJ2K_UINT32 dx;
    /** YRsiz: vertical separation of a sample of ith component with respect to the reference grid */
    NJ2K_UINT32 dy;
    /** data width */
    NJ2K_UINT32 w;
    /** data height */
    NJ2K_UINT32 h;
    /** x component offset compared to the whole image */
    NJ2K_UINT32 x0;
    /** y component offset compared to the whole image */
    NJ2K_UINT32 y0;
    /** precision: number of bits per component per pixel */
    NJ2K_UINT32 prec;
    /** obsolete: use prec instead */
    NJ2K_DEPRECATED_STRUCT_MEMBER(NJ2K_UINT32 bpp, "Use prec instead");
    /** signed (1) / unsigned (0) */
    NJ2K_UINT32 sgnd;
    /** number of decoded resolution */
    NJ2K_UINT32 resno_decoded;
    /** number of division by 2 of the out image compared to the original size of image */
    NJ2K_UINT32 factor;
    /** image component data */
    NJ2K_INT32 *data;
    /** alpha channel */
    NJ2K_UINT16 alpha;
} nj2k_image_comp_t;

/**
 * Defines image data and characteristics
 * */
typedef struct nj2k_image {
    /** XOsiz: horizontal offset from the origin of the reference grid to the left side of the image area */
    NJ2K_UINT32 x0;
    /** YOsiz: vertical offset from the origin of the reference grid to the top side of the image area */
    NJ2K_UINT32 y0;
    /** Xsiz: width of the reference grid */
    NJ2K_UINT32 x1;
    /** Ysiz: height of the reference grid */
    NJ2K_UINT32 y1;
    /** number of components in the image */
    NJ2K_UINT32 numcomps;
    /** color space: sRGB, Greyscale or YUV */
    NJ2K_COLOR_SPACE color_space;
    /** image components */
    nj2k_image_comp_t *comps;
    /** 'restricted' ICC profile */
    NJ2K_BYTE *icc_profile_buf;
    /** size of ICC profile */
    NJ2K_UINT32 icc_profile_len;
} nj2k_image_t;


/**
 * Component parameters structure used by the nj2k_image_create function
 * */
typedef struct nj2k_image_comptparm {
    /** XRsiz: horizontal separation of a sample of ith component with respect to the reference grid */
    NJ2K_UINT32 dx;
    /** YRsiz: vertical separation of a sample of ith component with respect to the reference grid */
    NJ2K_UINT32 dy;
    /** data width */
    NJ2K_UINT32 w;
    /** data height */
    NJ2K_UINT32 h;
    /** x component offset compared to the whole image */
    NJ2K_UINT32 x0;
    /** y component offset compared to the whole image */
    NJ2K_UINT32 y0;
    /** precision: number of bits per component per pixel */
    NJ2K_UINT32 prec;
    /** obsolete: use prec instead */
    NJ2K_DEPRECATED_STRUCT_MEMBER(NJ2K_UINT32 bpp, "Use prec instead");
    /** signed (1) / unsigned (0) */
    NJ2K_UINT32 sgnd;
} nj2k_image_cmptparm_t;


/*
==========================================================
   Information on the JPEG 2000 codestream
==========================================================
*/
/* QUITE EXPERIMENTAL FOR THE MOMENT */

/**
 * Index structure : Information concerning a packet inside tile
 * */
typedef struct nj2k_packet_info {
    /** packet start position (including SOP marker if it exists) */
    NJ2K_OFF_T start_pos;
    /** end of packet header position (including EPH marker if it exists)*/
    NJ2K_OFF_T end_ph_pos;
    /** packet end position */
    NJ2K_OFF_T end_pos;
    /** packet distortion */
    double disto;
} nj2k_packet_info_t;


/* UniPG>> */
/**
 * Marker structure
 * */
typedef struct nj2k_marker_info {
    /** marker type */
    unsigned short int type;
    /** position in codestream */
    NJ2K_OFF_T pos;
    /** length, marker val included */
    int len;
} nj2k_marker_info_t;
/* <<UniPG */

/**
 * Index structure : Information concerning tile-parts
*/
typedef struct nj2k_tp_info {
    /** start position of tile part */
    int tp_start_pos;
    /** end position of tile part header */
    int tp_end_header;
    /** end position of tile part */
    int tp_end_pos;
    /** start packet of tile part */
    int tp_start_pack;
    /** number of packets of tile part */
    int tp_numpacks;
} nj2k_tp_info_t;

/**
 * Index structure : information regarding tiles
*/
typedef struct nj2k_tile_info {
    /** value of thresh for each layer by tile cfr. Marcela   */
    double *thresh;
    /** number of tile */
    int tileno;
    /** start position */
    int start_pos;
    /** end position of the header */
    int end_header;
    /** end position */
    int end_pos;
    /** precinct number for each resolution level (width) */
    int pw[33];
    /** precinct number for each resolution level (height) */
    int ph[33];
    /** precinct size (in power of 2), in X for each resolution level */
    int pdx[33];
    /** precinct size (in power of 2), in Y for each resolution level */
    int pdy[33];
    /** information concerning packets inside tile */
    nj2k_packet_info_t *packet;
    /** number of pixels of the tile */
    int numpix;
    /** distortion of the tile */
    double distotile;
    /** number of markers */
    int marknum;
    /** list of markers */
    nj2k_marker_info_t *marker;
    /** actual size of markers array */
    int maxmarknum;
    /** number of tile parts */
    int num_tps;
    /** information concerning tile parts */
    nj2k_tp_info_t *tp;
} nj2k_tile_info_t;

/**
 * Index structure of the codestream
*/
typedef struct nj2k_codestream_info {
    /** maximum distortion reduction on the whole image (add for Marcela) */
    double D_max;
    /** packet number */
    int packno;
    /** writing the packet in the index with t2_encode_packets */
    int index_write;
    /** image width */
    int image_w;
    /** image height */
    int image_h;
    /** progression order */
    NJ2K_PROG_ORDER prog;
    /** tile size in x */
    int tile_x;
    /** tile size in y */
    int tile_y;
    /** */
    int tile_Ox;
    /** */
    int tile_Oy;
    /** number of tiles in X */
    int tw;
    /** number of tiles in Y */
    int th;
    /** component numbers */
    int numcomps;
    /** number of layer */
    int numlayers;
    /** number of decomposition for each component */
    int *numdecompos;
    /* UniPG>> */
    /** number of markers */
    int marknum;
    /** list of markers */
    nj2k_marker_info_t *marker;
    /** actual size of markers array */
    int maxmarknum;
    /* <<UniPG */
    /** main header position */
    int main_head_start;
    /** main header position */
    int main_head_end;
    /** codestream's size */
    int codestream_size;
    /** information regarding tiles inside image */
    nj2k_tile_info_t *tile;
} nj2k_codestream_info_t;

/* <----------------------------------------------------------- */
/* new output management of the codestream information and index */

/**
 * Tile-component coding parameters information
 */
typedef struct nj2k_tccp_info {
    /** component index */
    NJ2K_UINT32 compno;
    /** coding style */
    NJ2K_UINT32 csty;
    /** number of resolutions */
    NJ2K_UINT32 numresolutions;
    /** log2 of code-blocks width */
    NJ2K_UINT32 cblkw;
    /** log2 of code-blocks height */
    NJ2K_UINT32 cblkh;
    /** code-block coding style */
    NJ2K_UINT32 cblksty;
    /** discrete wavelet transform identifier: 0 = 9-7 irreversible, 1 = 5-3 reversible */
    NJ2K_UINT32 qmfbid;
    /** quantisation style */
    NJ2K_UINT32 qntsty;
    /** stepsizes used for quantization */
    NJ2K_UINT32 stepsizes_mant[NJ2K_J2K_MAXBANDS];
    /** stepsizes used for quantization */
    NJ2K_UINT32 stepsizes_expn[NJ2K_J2K_MAXBANDS];
    /** number of guard bits */
    NJ2K_UINT32 numgbits;
    /** Region Of Interest shift */
    NJ2K_INT32 roishift;
    /** precinct width */
    NJ2K_UINT32 prcw[NJ2K_J2K_MAXRLVLS];
    /** precinct height */
    NJ2K_UINT32 prch[NJ2K_J2K_MAXRLVLS];
}
nj2k_tccp_info_t;

/**
 * Tile coding parameters information
 */
typedef struct nj2k_tile_v2_info {

    /** number (index) of tile */
    int tileno;
    /** coding style */
    NJ2K_UINT32 csty;
    /** progression order */
    NJ2K_PROG_ORDER prg;
    /** number of layers */
    NJ2K_UINT32 numlayers;
    /** multi-component transform identifier */
    NJ2K_UINT32 mct;

    /** information concerning tile component parameters*/
    nj2k_tccp_info_t *tccp_info;

} nj2k_tile_info_v2_t;

/**
 * Information structure about the codestream (FIXME should be expand and enhance)
 */
typedef struct nj2k_codestream_info_v2 {
    /* Tile info */
    /** tile origin in x = XTOsiz */
    NJ2K_UINT32 tx0;
    /** tile origin in y = YTOsiz */
    NJ2K_UINT32 ty0;
    /** tile size in x = XTsiz */
    NJ2K_UINT32 tdx;
    /** tile size in y = YTsiz */
    NJ2K_UINT32 tdy;
    /** number of tiles in X */
    NJ2K_UINT32 tw;
    /** number of tiles in Y */
    NJ2K_UINT32 th;

    /** number of components*/
    NJ2K_UINT32 nbcomps;

    /** Default information regarding tiles inside image */
    nj2k_tile_info_v2_t m_default_tile_info;

    /** information regarding tiles inside image */
    nj2k_tile_info_v2_t *tile_info; /* FIXME not used for the moment */

} nj2k_codestream_info_v2_t;


/**
 * Index structure about a tile part
 */
typedef struct nj2k_tp_index {
    /** start position */
    NJ2K_OFF_T start_pos;
    /** end position of the header */
    NJ2K_OFF_T end_header;
    /** end position */
    NJ2K_OFF_T end_pos;

} nj2k_tp_index_t;

/**
 * Index structure about a tile
 */
typedef struct nj2k_tile_index {
    /** tile index */
    NJ2K_UINT32 tileno;

    /** number of tile parts */
    NJ2K_UINT32 nb_tps;
    /** current nb of tile part (allocated)*/
    NJ2K_UINT32 current_nb_tps;
    /** current tile-part index */
    NJ2K_UINT32 current_tpsno;
    /** information concerning tile parts */
    nj2k_tp_index_t *tp_index;

    /* UniPG>> */ /* NOT USED FOR THE MOMENT IN THE V2 VERSION */
    /** number of markers */
    NJ2K_UINT32 marknum;
    /** list of markers */
    nj2k_marker_info_t *marker;
    /** actual size of markers array */
    NJ2K_UINT32 maxmarknum;
    /* <<UniPG */

    /** packet number */
    NJ2K_UINT32 nb_packet;
    /** information concerning packets inside tile */
    nj2k_packet_info_t *packet_index;

} nj2k_tile_index_t;

/**
 * Index structure of the codestream (FIXME should be expand and enhance)
 */
typedef struct nj2k_codestream_index {
    /** main header start position (SOC position) */
    NJ2K_OFF_T main_head_start;
    /** main header end position (first SOT position) */
    NJ2K_OFF_T main_head_end;

    /** codestream's size */
    NJ2K_UINT64 codestream_size;

    /* UniPG>> */ /* NOT USED FOR THE MOMENT IN THE V2 VERSION */
    /** number of markers */
    NJ2K_UINT32 marknum;
    /** list of markers */
    nj2k_marker_info_t *marker;
    /** actual size of markers array */
    NJ2K_UINT32 maxmarknum;
    /* <<UniPG */

    /** */
    NJ2K_UINT32 nb_of_tiles;
    /** */
    nj2k_tile_index_t *tile_index; /* FIXME not used for the moment */

} nj2k_codestream_index_t;
/* -----------------------------------------------------------> */

/*
==========================================================
   Metadata from the JP2file
==========================================================
*/

/**
 * Info structure of the JP2 file
 * EXPERIMENTAL FOR THE MOMENT
 */
typedef struct nj2k_jp2_metadata {
    /** */
    NJ2K_INT32   not_used;

} nj2k_jp2_metadata_t;

/**
 * Index structure of the JP2 file
 * EXPERIMENTAL FOR THE MOMENT
 */
typedef struct nj2k_jp2_index {
    /** */
    NJ2K_INT32   not_used;

} nj2k_jp2_index_t;


#ifdef __cplusplus
extern "C" {
#endif


/*
==========================================================
   openjpeg version
==========================================================
*/

/* Get the version of the library: neo-j2k's version, "MAJOR.MINOR.PATCH" (opj_version gave
   OpenJPEG's). */
NJ2K_API const char * NJ2K_CALLCONV nj2k_version(void);

/*
==========================================================
   image functions definitions
==========================================================
*/

/**
 * Create an image
 *
 * @param numcmpts      number of components
 * @param cmptparms     components parameters
 * @param clrspc        image color space
 * @return returns      a new image structure if successful, returns NULL otherwise
 * */
NJ2K_API nj2k_image_t* NJ2K_CALLCONV nj2k_image_create(NJ2K_UINT32 numcmpts,
        nj2k_image_cmptparm_t *cmptparms, NJ2K_COLOR_SPACE clrspc);

/**
 * Deallocate any resources associated with an image
 *
 * @param image         image to be destroyed
 */
NJ2K_API void NJ2K_CALLCONV nj2k_image_destroy(nj2k_image_t *image);

/**
 * Creates an image without allocating memory for the image (used in the new version of the library).
 *
 * @param   numcmpts    the number of components
 * @param   cmptparms   the components parameters
 * @param   clrspc      the image color space
 *
 * @return  a new image structure if successful, NULL otherwise.
*/
NJ2K_API nj2k_image_t* NJ2K_CALLCONV nj2k_image_tile_create(NJ2K_UINT32 numcmpts,
        nj2k_image_cmptparm_t *cmptparms, NJ2K_COLOR_SPACE clrspc);

/**
 * Allocator for nj2k_image_t->comps[].data
 * To be paired with nj2k_image_data_free.
 *
 * @param   size    number of bytes to allocate
 *
 * @return  a new pointer if successful, NULL otherwise.
 * @since 2.2.0
*/
NJ2K_API void* NJ2K_CALLCONV nj2k_image_data_alloc(NJ2K_SIZE_T size);

/**
 * Destructor for nj2k_image_t->comps[].data
 * To be paired with nj2k_image_data_alloc.
 *
 * @param   ptr    Pointer to free
 *
 * @since 2.2.0
*/
NJ2K_API void NJ2K_CALLCONV nj2k_image_data_free(void* ptr);

/*
==========================================================
   stream functions definitions
==========================================================
*/

/**
 * Creates an abstract stream. This function does nothing except allocating memory and initializing the abstract stream.
 *
 * @param   p_is_input      if set to true then the stream will be an input stream, an output stream else.
 *
 * @return  a stream object.
*/
NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_default_create(
    NJ2K_BOOL p_is_input);

/**
 * Creates an abstract stream. This function does nothing except allocating memory and initializing the abstract stream.
 *
 * @param   p_buffer_size  FIXME DOC
 * @param   p_is_input      if set to true then the stream will be an input stream, an output stream else.
 *
 * @return  a stream object.
*/
NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create(NJ2K_SIZE_T p_buffer_size,
        NJ2K_BOOL p_is_input);

/**
 * Destroys a stream created by nj2k_create_stream. This function does NOT close the abstract stream. If needed the user must
 * close its own implementation of the stream.
 *
 * @param   p_stream    the stream to destroy.
 */
NJ2K_API void NJ2K_CALLCONV nj2k_stream_destroy(nj2k_stream_t* p_stream);

/**
 * Sets the given function to be used as a read function.
 * @param       p_stream    the stream to modify
 * @param       p_function  the function to use a read function.
*/
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_read_function(nj2k_stream_t* p_stream,
        nj2k_stream_read_fn p_function);

/**
 * Sets the given function to be used as a write function.
 * @param       p_stream    the stream to modify
 * @param       p_function  the function to use a write function.
*/
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_write_function(nj2k_stream_t* p_stream,
        nj2k_stream_write_fn p_function);

/**
 * Sets the given function to be used as a skip function.
 * @param       p_stream    the stream to modify
 * @param       p_function  the function to use a skip function.
*/
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_skip_function(nj2k_stream_t* p_stream,
        nj2k_stream_skip_fn p_function);

/**
 * Sets the given function to be used as a seek function, the stream is then seekable,
 * using SEEK_SET behavior.
 * @param       p_stream    the stream to modify
 * @param       p_function  the function to use a skip function.
*/
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_seek_function(nj2k_stream_t* p_stream,
        nj2k_stream_seek_fn p_function);

/**
 * Sets the given data to be used as a user data for the stream.
 * @param       p_stream    the stream to modify
 * @param       p_data      the data to set.
 * @param       p_function  the function to free p_data when nj2k_stream_destroy() is called.
*/
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_user_data(nj2k_stream_t* p_stream,
        void * p_data, nj2k_stream_free_user_data_fn p_function);

/**
 * Sets the length of the user data for the stream.
 * @param p_stream    the stream to modify
 * @param data_length length of the user_data.
*/
NJ2K_API void NJ2K_CALLCONV nj2k_stream_set_user_data_length(
    nj2k_stream_t* p_stream, NJ2K_UINT64 data_length);

/**
 * Create a stream from a file identified with its filename with default parameters (helper function)
 * @param fname             the filename of the file to stream
 * @param p_is_read_stream  whether the stream is a read stream (true) or not (false)
*/
NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create_default_file_stream(
    const char *fname, NJ2K_BOOL p_is_read_stream);

/** Create a stream from a file identified with its filename with a specific buffer size
 * @param fname             the filename of the file to stream
 * @param p_buffer_size     size of the chunk used to stream
 * @param p_is_read_stream  whether the stream is a read stream (true) or not (false)
*/
NJ2K_API nj2k_stream_t* NJ2K_CALLCONV nj2k_stream_create_file_stream(
    const char *fname,
    NJ2K_SIZE_T p_buffer_size,
    NJ2K_BOOL p_is_read_stream);

/*
==========================================================
   event manager functions definitions
==========================================================
*/
/**
 * Set the info handler use by openjpeg.
 * @param p_codec       the codec previously initialise
 * @param p_callback    the callback function which will be used
 * @param p_user_data   client object where will be returned the message
*/
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_info_handler(nj2k_codec_t * p_codec,
        nj2k_msg_callback p_callback,
        void * p_user_data);
/**
 * Set the warning handler use by openjpeg.
 * @param p_codec       the codec previously initialise
 * @param p_callback    the callback function which will be used
 * @param p_user_data   client object where will be returned the message
*/
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_warning_handler(nj2k_codec_t * p_codec,
        nj2k_msg_callback p_callback,
        void * p_user_data);
/**
 * Set the error handler use by openjpeg.
 * @param p_codec       the codec previously initialise
 * @param p_callback    the callback function which will be used
 * @param p_user_data   client object where will be returned the message
*/
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_error_handler(nj2k_codec_t * p_codec,
        nj2k_msg_callback p_callback,
        void * p_user_data);

/*
==========================================================
   codec functions definitions
==========================================================
*/

/**
 * Creates a J2K/JP2 decompression structure
 * @param format        Decoder to select
 *
 * @return Returns a handle to a decompressor if successful, returns NULL otherwise
 * */
NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_decompress(
    NJ2K_CODEC_FORMAT format);

/**
 * Destroy a decompressor handle
 *
 * @param   p_codec         decompressor handle to destroy
 */
NJ2K_API void NJ2K_CALLCONV nj2k_destroy_codec(nj2k_codec_t * p_codec);

/**
 * Read after the codestream if necessary
 * @param   p_codec         the JPEG2000 codec to read.
 * @param   p_stream        the JPEG2000 stream.
 *
 * neo-j2k: reads nothing more, also after a JP2 file's codestream (DP-13), and returns NJ2K_TRUE;
 * nj2k_get_status() keeps the status of the decode before it (NJ2K_TRUNCATED stays). Without a
 * stream it fails with NJ2K_E_ARGUMENT.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_decompress(nj2k_codec_t *p_codec,
        nj2k_stream_t *p_stream);


/**
 * Set decoding parameters to default values
 * @param parameters Decompression parameters
 */
NJ2K_API void NJ2K_CALLCONV nj2k_set_default_decoder_parameters(
    nj2k_dparameters_t *parameters);

/**
 * Setup the decoder with decompression parameters provided by the user and with the message handler
 * provided by the user.
 *
 * @param p_codec       decompressor handler
 * @param parameters    decompression parameters
 *
 * @return true         if the decoder is correctly set
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_setup_decoder(nj2k_codec_t *p_codec,
        nj2k_dparameters_t *parameters);

/**
 * Set strict decoding parameter for this decoder.
 * If strict decoding is enabled, partial bit streams will fail to decode, and
 * the check for invalid TPSOT values added in https://github.com/uclouvain/openjpeg/pull/514
 * will be disabled.
 * If strict decoding is disabled, the decoder will decode partial
 * bitstreams as much as possible without erroring, and the TPSOT fixing logic
 * will be enabled.
 *
 * @param p_codec       decompressor handler
 * @param strict        NJ2K_TRUE to enable strict decoding, NJ2K_FALSE to disable
 *
 * @return true         if the decoder is correctly set
 */

NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_strict_mode(nj2k_codec_t *p_codec,
        NJ2K_BOOL strict);

/**
 * Fills decoder limits with a profile.
 *
 * @param p_limits      the limits to fill.
 * @param profile       NJ2K_DECODER_LIMITS_NONE, NJ2K_DECODER_LIMITS_SL_TEXTURE or, in builds
 *                      with the OpenSim profile module, NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE.
 *
 * @return NJ2K_TRUE if the profile is known. With an unknown profile the limits are set to
 *         NJ2K_DECODER_LIMITS_NONE and NJ2K_FALSE is returned.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_limits_init(
    nj2k_decoder_limits_t *p_limits, NJ2K_UINT32 profile);

/**
 * Sets the resource limits of a decoder (see nj2k_decoder_limits_t).
 * Call it after nj2k_setup_decoder() and before nj2k_read_header(). The limits are copied.
 *
 * @param p_codec       decompressor handler
 * @param p_limits      the limits
 *
 * @return NJ2K_TRUE if the limits are set
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_limits(nj2k_codec_t *p_codec,
        const nj2k_decoder_limits_t *p_limits);

/**
 * Allocates worker threads for the compressor/decompressor.
 *
 * By default, only the main thread is used. If this function is not used,
 * but the NJ2K_NUM_THREADS environment variable is set, its value will be
 * used to initialize the number of threads. The value can be either an integer
 * number, or "ALL_CPUS". If NJ2K_NUM_THREADS is set and this function is called,
 * this function will override the behaviour of the environment variable.
 *
 * This function must be called after nj2k_setup_decoder() and
 * before nj2k_read_header() for the decoding side, or after nj2k_setup_encoder()
 * and before nj2k_start_compress() for the encoding side.
 *
 * @param p_codec       decompressor or compressor handler
 * @param num_threads   number of threads.
 *
 * @return NJ2K_TRUE     if the function is successful.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_codec_set_threads(nj2k_codec_t *p_codec,
        int num_threads);

/**
 * Decodes an image header.
 *
 * @param   p_stream        the jpeg2000 stream.
 * @param   p_codec         the jpeg2000 codec to read.
 * @param   p_image         the image structure initialized with the characteristics of encoded image.
 *
 * @return true             if the main header of the codestream and the JP2 header is correctly read.
 *
 * neo-j2k: on failure nj2k_get_status() says why. NJ2K_E_NEED_DATA means the bytes ended before
 * the headers did, which is no damage: read the header again from a stream with more bytes, on a
 * new codec. The decoder limits are checked here, before anything they bound is allocated.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_header(nj2k_stream_t *p_stream,
        nj2k_codec_t *p_codec,
        nj2k_image_t **p_image);


/** Restrict the number of components to decode.
 *
 * This function should be called after nj2k_read_header().
 *
 * This function enables to restrict the set of decoded components to the
 * specified indices.
 * Note that the current implementation (apply_color_transforms == NJ2K_FALSE)
 * is such that neither the multi-component transform at codestream level,
 * nor JP2 channel transformations will be applied.
 * Consequently the indices are relative to the codestream.
 *
 * Note: nj2k_decode_tile_data() should not be used together with nj2k_set_decoded_components().
 *
 * @param   p_codec         the jpeg2000 codec to read.
 * @param   numcomps        Size of the comps_indices array.
 * @param   comps_indices   Array of numcomps values representing the indices
 *                          of the components to decode (relative to the
 *                          codestream, starting at 0)
 * @param   apply_color_transforms Whether multi-component transform at codestream level
 *                                 or JP2 channel transformations should be applied.
 *                                 Currently this parameter should be set to NJ2K_FALSE.
 *                                 Setting it to NJ2K_TRUE will result in an error.
 *
 * @return NJ2K_TRUE         in case of success.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decoded_components(nj2k_codec_t *p_codec,
        NJ2K_UINT32 numcomps,
        const NJ2K_UINT32* comps_indices,
        NJ2K_BOOL apply_color_transforms);

/**
 * Sets the given area to be decoded. This function should be called right after nj2k_read_header and before any tile header reading.
 *
 * The coordinates passed to this function should be expressed in the reference grid,
 * that is to say at the highest resolution level, even if requesting the image at lower
 * resolution levels.
 *
 * Note: If p_start_x, p_start_y, p_end_x, p_end_y are all 0, then the whole image is decoded.
 *
 * Generally nj2k_set_decode_area() should be followed by nj2k_decode(), and the
 * codec cannot be re-used.
 * In the particular case of an image made of a single tile, several sequences of
 * calls to opoj_set_decode_area() and nj2k_decode() are allowed, and will bring
 * performance improvements when reading an image by chunks.
 *
 * @param   p_codec         the jpeg2000 codec.
 * @param   p_image         the decoded image previously set by nj2k_read_header
 * @param   p_start_x       the left position of the rectangle to decode (in image coordinates).
 * @param   p_end_x         the right position of the rectangle to decode (in image coordinates).
 * @param   p_start_y       the up position of the rectangle to decode (in image coordinates).
 * @param   p_end_y         the bottom position of the rectangle to decode (in image coordinates).
 *
 * @return  true            if the area could be set.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decode_area(nj2k_codec_t *p_codec,
        nj2k_image_t* p_image,
        NJ2K_INT32 p_start_x, NJ2K_INT32 p_start_y,
        NJ2K_INT32 p_end_x, NJ2K_INT32 p_end_y);

/**
 * Decode an image from a JPEG-2000 codestream
 *
 * @param p_decompressor    decompressor handle
 * @param p_stream          Input buffer stream
 * @param p_image           the decoded image
 * @return                  true if success, otherwise false
 * */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode(nj2k_codec_t *p_decompressor,
        nj2k_stream_t *p_stream,
        nj2k_image_t *p_image);

/**
 * Get the decoded tile from the codec
 *
 * @param   p_codec         the jpeg2000 codec.
 * @param   p_stream        input stream
 * @param   p_image         output image
 * @param   tile_index      index of the tile which will be decode
 *
 * @return                  true if success, otherwise false
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_decoded_tile(nj2k_codec_t *p_codec,
        nj2k_stream_t *p_stream,
        nj2k_image_t *p_image,
        NJ2K_UINT32 tile_index);

/**
 * Set the resolution factor of the decoded image
 * @param   p_codec         the jpeg2000 codec.
 * @param   res_factor      resolution factor to set
 *
 * @return                  true if success, otherwise false
 *
 * neo-j2k, as OpenJPEG with the fork's stage 1a fix: before nj2k_read_header() the factor is kept
 * and checked when the header is read, and the call returns NJ2K_FALSE (it could not be checked
 * yet); after it the factor is checked against every component first, and a refused factor leaves
 * the decoder as it was. A factor at or above a component's number of resolutions is refused
 * (NJ2K_E_LIMIT) under the default reduce policy, NJ2K_REDUCE_EXACT, and lowered to the coarsest
 * level the codestream has under NJ2K_REDUCE_CLAMP (nj2k_decoder_set_reduce_policy()). A codec
 * decoding from memory (nj2k_read_header_memory(), nj2k_decode_memory()) takes the factor this way
 * too after a failed header read: it is kept for the next codestream, whose header read starts
 * afresh, and the call returns NJ2K_FALSE. After a failed decode the codec still holds the header,
 * and the factor is checked against it at once.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_decoded_resolution_factor(
    nj2k_codec_t *p_codec, NJ2K_UINT32 res_factor);

/**
 * Writes a tile with the given data.
 *
 * @param   p_codec             the jpeg2000 codec.
 * @param   p_tile_index        the index of the tile to write. At the moment, the tiles must be written from 0 to n-1 in sequence.
 * @param   p_data              pointer to the data to write. Data is arranged in sequence, data_comp0, then data_comp1, then ... NO INTERLEAVING should be set.
 * @param   p_data_size         this value os used to make sure the data being written is correct. The size must be equal to the sum for each component of
 *                              tile_width * tile_height * component_size. component_size can be 1,2 or 4 bytes, depending on the precision of the given component.
 * @param   p_stream            the stream to write data to.
 *
 * @return  true if the data could be written.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_write_tile(nj2k_codec_t *p_codec,
        NJ2K_UINT32 p_tile_index,
        NJ2K_BYTE * p_data,
        NJ2K_UINT32 p_data_size,
        nj2k_stream_t *p_stream);

/**
 * Reads a tile header. This function is compulsory and allows one to know the size of the tile that will be decoded.
 * The user may need to refer to the image got by nj2k_read_header to understand the size being taken by the tile.
 *
 * @param   p_codec         the jpeg2000 codec.
 * @param   p_tile_index    pointer to a value that will hold the index of the tile being decoded, in case of success.
 * @param   p_data_size     pointer to a value that will hold the maximum size of the decoded data, in case of success. In case
 *                          of truncated codestreams, the actual number of bytes decoded may be lower. The computation of the size is the same
 *                          as depicted in nj2k_write_tile.
 * @param   p_tile_x0       pointer to a value that will hold the x0 pos of the tile (in the image).
 * @param   p_tile_y0       pointer to a value that will hold the y0 pos of the tile (in the image).
 * @param   p_tile_x1       pointer to a value that will hold the x1 pos of the tile (in the image).
 * @param   p_tile_y1       pointer to a value that will hold the y1 pos of the tile (in the image).
 * @param   p_nb_comps      pointer to a value that will hold the number of components in the tile.
 * @param   p_should_go_on  pointer to a boolean that will hold the fact that the decoding should go on. In case the
 *                          codestream is over at the time of the call, the value will be set to false. The user should then stop
 *                          the decoding.
 * @param   p_stream        the stream to decode.
 * @return  true            if the tile header could be decoded. In case the decoding should end, the returned value is still true.
 *                          returning false may be the result of a shortage of memory or an internal error.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_tile_header(nj2k_codec_t *p_codec,
        nj2k_stream_t * p_stream,
        NJ2K_UINT32 * p_tile_index,
        NJ2K_UINT32 * p_data_size,
        NJ2K_INT32 * p_tile_x0, NJ2K_INT32 * p_tile_y0,
        NJ2K_INT32 * p_tile_x1, NJ2K_INT32 * p_tile_y1,
        NJ2K_UINT32 * p_nb_comps,
        NJ2K_BOOL * p_should_go_on);

/**
 * Reads a tile data. This function is compulsory and allows one to decode tile data. nj2k_read_tile_header should be called before.
 * The user may need to refer to the image got by nj2k_read_header to understand the size being taken by the tile.
 *
 * Note: nj2k_decode_tile_data() should not be used together with nj2k_set_decoded_components().
 *
 * @param   p_codec         the jpeg2000 codec.
 * @param   p_tile_index    the index of the tile being decoded, this should be the value set by nj2k_read_tile_header.
 * @param   p_data          pointer to a memory block that will hold the decoded data.
 * @param   p_data_size     size of p_data. p_data_size should be bigger or equal to the value set by nj2k_read_tile_header.
 * @param   p_stream        the stream to decode.
 *
 * @return  true            if the data could be decoded.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode_tile_data(nj2k_codec_t *p_codec,
        NJ2K_UINT32 p_tile_index,
        NJ2K_BYTE * p_data,
        NJ2K_UINT32 p_data_size,
        nj2k_stream_t *p_stream);

/* COMPRESSION FUNCTIONS*/

/**
 * Creates a J2K/JP2 compression structure
 * @param   format      Coder to select. The core encoder writes J2K codestreams only: the other
 *                      formats give NULL (the j2k-codestream module).
 * @return              Returns a handle to a compressor if successful, returns NULL otherwise
 */
NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_compress(NJ2K_CODEC_FORMAT format);

/**
Set encoding parameters to default values, that means :
<ul>
<li>Lossless
<li>1 tile
<li>Size of precinct : 2^15 x 2^15 (means 1 precinct)
<li>Size of code-block : 64 x 64
<li>Number of resolutions: 6
<li>No SOP marker in the codestream
<li>No EPH marker in the codestream
<li>No sub-sampling in x or y direction
<li>No mode switch activated
<li>Progression order: LRCP
<li>No index file
<li>No ROI upshifted
<li>No offset of the origin of the image
<li>No offset of the origin of the tiles
<li>Reversible DWT 5-3
</ul>
@param parameters Compression parameters
*/
NJ2K_API void NJ2K_CALLCONV nj2k_set_default_encoder_parameters(
    nj2k_cparameters_t *parameters);

/**
 * Setup the encoder parameters using the current image and using user parameters.
 *
 * The core encoder takes the settings of the Second Life viewer's upload encode and refuses the
 * rest with NJ2K_E_UNSUPPORTED (nj2k_get_status): see doc/api.md, "The encoder". The caller's
 * parameters are written back where OpenJPEG writes them (no layers asked: one lossless layer; the
 * codestream size the last rate gives in max_cs_size).
 * @param p_codec       Compressor handle
 * @param parameters    Compression parameters
 * @param image         Input filled image
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_setup_encoder(nj2k_codec_t *p_codec,
        nj2k_cparameters_t *parameters,
        nj2k_image_t *image);


/**
 * Specify extra options for the encoder.
 *
 * This may be called after nj2k_setup_encoder() and before nj2k_start_compress()
 *
 * This is the way to add new options in a fully ABI compatible way, without
 * extending the nj2k_cparameters_t structure.
 *
 * The core encoder takes PLT=NO and TLM=NO (what it does); the other options belong to the j2k-extra
 * module (NJ2K_E_UNSUPPORTED). OpenJPEG's own are:
 * <ul>
 * <li>PLT=YES/NO. Defaults to NO. If set to YES, PLT marker segments,
 *     indicating the length of each packet in the tile-part header, will be
 *     written. Since 2.4.0</li>
 * <li>TLM=YES/NO. Defaults to NO (except for Cinema and IMF profiles).
 *     If set to YES, TLM marker segments, indicating the length of each
 *     tile-part part will be written. Since 2.4.0</li>
 * <li>GUARD_BITS=value. Number of guard bits in [0,7] range. Default value is 2.
 *     1 may be used sometimes (like in SMPTE DCP Bv2.1 Application Profile for 2K images).
 *     Since 2.5.0</li>
 * </ul>
 *
 * @param p_codec       Compressor handle
 * @param p_options     Compression options. This should be a NULL terminated
 *                      array of strings. Each string is of the form KEY=VALUE.
 *
 * @return NJ2K_TRUE in case of success.
 * @since 2.4.0
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encoder_set_extra_options(
    nj2k_codec_t *p_codec,
    const char* const* p_options);

/**
 * Start to compress the current image: writes the main header to the stream. neo-j2k copies the
 * image's sample planes here and leaves them to the caller, where OpenJPEG takes them.
 * @param p_codec       Compressor handle
 * @param p_image       Input filled image
 * @param p_stream      Output stream (a write function; skip and seek are not called)
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_start_compress(nj2k_codec_t *p_codec,
        nj2k_image_t * p_image,
        nj2k_stream_t *p_stream);

/**
 * End to compress the current image.
 * @param p_codec       Compressor handle
 * @param p_stream      Input stgream
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_end_compress(nj2k_codec_t *p_codec,
        nj2k_stream_t *p_stream);

/**
 * Encode an image into a JPEG-2000 codestream
 * @param p_codec       compressor handle
 * @param p_stream      Output buffer stream
 *
 * @return              Returns true if successful, returns false otherwise
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encode(nj2k_codec_t *p_codec,
        nj2k_stream_t *p_stream);
/*
==========================================================
   codec output functions definitions
==========================================================
*/
/* EXPERIMENTAL FUNCTIONS FOR NOW, USED ONLY IN J2K_DUMP*/

/**
Destroy Codestream information after compression or decompression
@param cstr_info Codestream information structure
*/
NJ2K_API void NJ2K_CALLCONV nj2k_destroy_cstr_info(nj2k_codestream_info_v2_t
        **cstr_info);


/**
 * Dump the codec information into the output stream
 *
 * @param   p_codec         the jpeg2000 codec.
 * @param   info_flag       type of information dump.
 * @param   output_stream   output stream where dump the information gotten from the codec.
 *
 */
NJ2K_API void NJ2K_CALLCONV nj2k_dump_codec(nj2k_codec_t *p_codec,
        NJ2K_INT32 info_flag,
        FILE* output_stream);

/**
 * Get the codestream information from the codec
 *
 * @param   p_codec         the jpeg2000 codec.
 *
 * @return                  a pointer to a codestream information structure.
 *
 */
NJ2K_API nj2k_codestream_info_v2_t* NJ2K_CALLCONV nj2k_get_cstr_info(
    nj2k_codec_t *p_codec);

/**
 * Get the codestream index from the codec
 *
 * @param   p_codec         the jpeg2000 codec.
 *
 * @return                  a pointer to a codestream index structure.
 *
 */
NJ2K_API nj2k_codestream_index_t * NJ2K_CALLCONV nj2k_get_cstr_index(
    nj2k_codec_t *p_codec);

NJ2K_API void NJ2K_CALLCONV nj2k_destroy_cstr_index(nj2k_codestream_index_t
        **p_cstr_index);


/**
 * Get the JP2 file information from the codec FIXME
 *
 * @param   p_codec         the jpeg2000 codec.
 *
 * @return                  a pointer to a JP2 metadata structure.
 *
 */
NJ2K_API nj2k_jp2_metadata_t* NJ2K_CALLCONV nj2k_get_jp2_metadata(
    nj2k_codec_t *p_codec);

/**
 * Get the JP2 file index from the codec FIXME
 *
 * @param   p_codec         the jpeg2000 codec.
 *
 * @return                  a pointer to a JP2 index structure.
 *
 */
NJ2K_API nj2k_jp2_index_t* NJ2K_CALLCONV nj2k_get_jp2_index(nj2k_codec_t *p_codec);


/*
==========================================================
   MCT functions
==========================================================
*/

/**
 * Sets the MCT matrix to use.
 *
 * @param   parameters      the parameters to change.
 * @param   pEncodingMatrix the encoding matrix.
 * @param   p_dc_shift      the dc shift coefficients to use.
 * @param   pNbComp         the number of components of the image.
 *
 * @return  true if the parameters could be set.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_set_MCT(nj2k_cparameters_t *parameters,
        NJ2K_FLOAT32 * pEncodingMatrix,
        NJ2K_INT32 * p_dc_shift,
        NJ2K_UINT32 pNbComp);

/*
==========================================================
   Thread functions
==========================================================
*/

/** Returns if the library is built with thread support.
 * NJ2K_TRUE if mutex, condition, thread, thread pool are available.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_has_thread_support(void);

/** Return the number of virtual CPUs */
NJ2K_API int NJ2K_CALLCONV nj2k_get_num_cpus(void);

/*
==========================================================
   neo-j2k extensions (no OpenJPEG counterpart; draft)
==========================================================
*/

/** The longest string nj2k_engine_string() returns, without its terminator: it fits a 32-byte
 * field, such as the one the viewer's texture cache keeps the decoder's name in. */
#define NJ2K_ENGINE_STRING_MAX 31

/**
 * The decoder's name and release line, "neo-j2k MAJOR.MINOR", for logs and an About box. A
 * compile-time constant of at most NJ2K_ENGINE_STRING_MAX characters; safe to call before
 * main() and from any thread. It names no version of the decoded output, and neither does
 * nj2k_version() (opj_version() through the compatibility header), which changes with every
 * release: a host that caches decoded images keys the cache on its own decoder-output version,
 * bumped when the pixels can change (owner decision D13).
 */
NJ2K_API const char * NJ2K_CALLCONV nj2k_engine_string(void);

/**
 * Whether this CPU runs the library as built: 1 if the processor and the operating system
 * support every instruction set the build uses (AVX2, FMA, BMI1/2, LZCNT, MOVBE and F16C for
 * the x86-64-v3 build; nothing beyond x86-64 for the scalar build), else 0. It is compiled for
 * baseline x86-64, so a host can call it before anything else in the library.
 */
NJ2K_API int NJ2K_CALLCONV nj2k_cpu_supported(void);

/**
 * OpenSim textures, from neo-j2k's OpenSim profile module (built unless the build turns
 * NJ2K_PROFILE_OPENSIM off): the Second Life profile (NJ2K_DECODER_LIMITS_SL_TEXTURE) with
 * 16,384 pixels per side at the decoded resolution. In a build without the module,
 * nj2k_decoder_limits_init() does not know it, sets no limits and returns NJ2K_FALSE.
 */
#define NJ2K_DECODER_LIMITS_OPENSIM_TEXTURE  2

/**
 * The outcome of a codec's last call, from nj2k_get_status(). Zero and above is success, below
 * zero failure; callers treat an unknown negative value as failure and an unknown positive one as
 * success, so later versions can add statuses.
 */
typedef NJ2K_INT32 nj2k_status_t;
/** Done. */
#define NJ2K_OK                   0
/** Decoded from the bytes given, every one of them used, and the codestream goes on past them:
 * the image holds the layers and code-block passes that arrived (decoding, stage 1c). */
#define NJ2K_TRUNCATED            1
/** The bytes end before what the call needs is complete: the main header, the first tile-part
 * header, or the first packet of the requested resolutions. Nothing was found damaged; with more
 * bytes the call can succeed. A host that already holds every byte of the file treats it as
 * damage. */
#define NJ2K_E_NEED_DATA        (-1)
/** An argument is invalid (a null pointer, an unknown value), or the call does not apply to this
 * codec. */
#define NJ2K_E_ARGUMENT         (-2)
/** The bytes are not a JPEG 2000 codestream (or JP2/JPH file, for a JP2 codec). */
#define NJ2K_E_NOT_JPEG2000     (-3)
/** A header is invalid. */
#define NJ2K_E_HEADER           (-4)
/** Valid, but not supported: JPEG 2000 Part 2 extensions; HT code-blocks until the HT module; a
 * JP2 file read from a stream whose length is not given (nj2k_stream_set_user_data_length()), which
 * OpenJPEG cannot read either. */
#define NJ2K_E_UNSUPPORTED      (-5)
/** A decoder limit refuses the codestream, or the reduce factor does: under NJ2K_REDUCE_EXACT,
 * or, under either policy, a tile whose own COD or COC leaves a component no more resolutions
 * than the factor in effect. */
#define NJ2K_E_LIMIT            (-6)
/** An allocation failed; the codec can still be destroyed. */
#define NJ2K_E_NO_MEMORY        (-7)
/** The coded data is damaged where the requested resolutions needed it (decoding, stage 1c). */
#define NJ2K_E_DATA             (-8)
/** An output buffer or image does not fit the decode. */
#define NJ2K_E_OUTPUT           (-9)
/** The call came out of order: before nj2k_read_header(), after a failure, or twice. */
#define NJ2K_E_STATE           (-10)

/**
 * The status of the codec's last call that reports one: nj2k_setup_decoder(),
 * nj2k_decoder_set_strict_mode(), nj2k_decoder_set_limits(), nj2k_decoder_set_reduce_policy(),
 * nj2k_read_header(), nj2k_set_decoded_resolution_factor(), nj2k_set_decoded_components(),
 * nj2k_get_decoded_dimensions(), nj2k_get_cstr_info(), and the decoding functions but
 * nj2k_end_decompress(), which keeps the status of the decode before it (it reads nothing).
 * NJ2K_OK for a codec that has made none of these calls yet; NJ2K_E_ARGUMENT for a null codec.
 */
NJ2K_API nj2k_status_t NJ2K_CALLCONV nj2k_get_status(nj2k_codec_t *p_codec);

/** A short English description of a status ("need more data"), a static string. */
NJ2K_API const char * NJ2K_CALLCONV nj2k_status_string(nj2k_status_t status);

/**
 * A host's allocator for a codec's own memory, the codec object included. allocate returns a
 * block of at least size bytes aligned to alignment (a power of two, at most 4,096), or NULL;
 * release gets back a block with the size it was asked for. (The members are not called malloc
 * and free, which debug and leak-checking headers define as macros.) The images, streams and
 * codestream information that the API hands to the caller come from the C runtime's heap, as
 * OpenJPEG's do, since the functions that destroy them take no codec. Every structure of
 * neo-j2k's own that may grow in later versions starts with its size: set struct_size to
 * sizeof(nj2k_allocator_t).
 */
typedef struct nj2k_allocator {
    NJ2K_SIZE_T struct_size;
    void * (*allocate)(void *user, NJ2K_SIZE_T size, NJ2K_SIZE_T alignment);
    void (*release)(void *user, void *ptr, NJ2K_SIZE_T size);
    void *user;
} nj2k_allocator_t;

/**
 * nj2k_create_decompress() with the codec's memory from the host's allocator (copied; its user
 * pointer must stay valid until the codec is destroyed). NULL when the allocator is incomplete
 * or the codec cannot be allocated.
 */
NJ2K_API nj2k_codec_t* NJ2K_CALLCONV nj2k_create_decompress_with_allocator(
    NJ2K_CODEC_FORMAT format, const nj2k_allocator_t *allocator);

/** Reduce policies, for nj2k_decoder_set_reduce_policy(). */
/** A reduce factor at or above a component's number of resolutions is refused (NJ2K_E_LIMIT),
 * as OpenJPEG does; the default. */
#define NJ2K_REDUCE_EXACT 0
/** Such a factor is lowered to the coarsest level the main header gives every component; the
 * factor used is the components' factor in the image (nj2k_get_decoded_dimensions()). A tile
 * whose own COD or COC leaves a component no more resolutions than that factor has no level of
 * the image's size: it is refused (NJ2K_E_LIMIT), as under NJ2K_REDUCE_EXACT. */
#define NJ2K_REDUCE_CLAMP 1

/**
 * Sets the reduce policy, before or after nj2k_read_header(); it applies to the reduce factor
 * when it is next checked.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_reduce_policy(nj2k_codec_t *p_codec,
        NJ2K_UINT32 policy);

/**
 * Sets factor, x0, y0, w and h of every component of p_image, the image nj2k_read_header() gave,
 * to what nj2k_decode() delivers at the codec's reduce factor and policy, without decoding, so a
 * host can size its buffers first. It checks what decoding checks before it allocates: the reduce
 * factor, the decoded side and the working memory against the decoder limits. Call it after
 * nj2k_read_header() and nj2k_set_decoded_resolution_factor(), or after nj2k_read_header_memory()
 * with the image it described (its components are at the factor in effect; after setting another
 * factor, read the header again).
 *
 * @return NJ2K_TRUE if p_image is set; on failure p_image is unchanged and nj2k_get_status() says why.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_decoded_dimensions(nj2k_codec_t *p_codec,
        nj2k_image_t *p_image);

/*
 * Decoding from memory, codestream after codestream (neo-j2k's own; no OpenJPEG counterpart).
 *
 * One codec per thread decodes any number of codestreams held in memory: nj2k_read_header_memory()
 * reads a header (or keeps the one it holds when the bytes start with the same main header) and
 * nj2k_decode_memory() decodes into the caller's own buffers. The caller's bytes are read where
 * they lie during the call, never written and never copied whole: the codec copies only the main
 * header (to know it again), the tile-part headers' marker segments, each code-block's bytes (into
 * a padded buffer for the arithmetic decoder) and the tile-parts of a tile that has several. No
 * image is allocated, and the codec keeps its memory for the next codestream
 * (nj2k_decoder_set_retained_bytes()): after the first decodes of a given size it makes no heap
 * call at all. The settings stay from one codestream to the next: strict mode, limits, reduce
 * policy, the layers (cp_layer of nj2k_setup_decoder()) and the reduce factor
 * (nj2k_set_decoded_resolution_factor(), before or after the header read). A component selection
 * (nj2k_set_decoded_components() after nj2k_read_header_memory()) holds for the decodes of that
 * header, until the next nj2k_read_header_memory() or a decode that reads another header. Decoded
 * samples are those of nj2k_decode(), bit for bit.
 */

/* Sample types of nj2k_output_t. */
/** The decoded samples, as nj2k_decode() puts them in an image's planes. */
#define NJ2K_SAMPLE_I32 0
/** Their low 8 bits (what an 8-bit copy of the image's planes keeps). */
#define NJ2K_SAMPLE_U8  1
/** Their low 16 bits. */
#define NJ2K_SAMPLE_U16 2

/**
 * Where one component goes: sample (x, y) of the decoded component (x < its w, y < its h, as
 * nj2k_read_header_memory() describes it) is written at (char *)data + y * row_stride +
 * x * sample_stride. A negative row_stride writes the image bottom-up (data then points at the
 * last row); a sample_stride above the sample's size interleaves components (data then points at
 * the component's first sample). Several planes may take the same component. The structure is an
 * array element and keeps its layout; a later version that needs more for a plane says so in
 * nj2k_output_t, which starts with its size.
 */
typedef struct nj2k_plane {
    void *data;
    NJ2K_INT64 row_stride;
    NJ2K_UINT32 sample_stride;
    NJ2K_UINT32 component;
} nj2k_plane_t;

/** The caller's buffers for nj2k_decode_memory(). */
typedef struct nj2k_output {
    NJ2K_SIZE_T struct_size;        /**< sizeof(nj2k_output_t) */
    NJ2K_UINT32 sample_type;        /**< NJ2K_SAMPLE_I32, NJ2K_SAMPLE_U8 or NJ2K_SAMPLE_U16 */
    NJ2K_UINT32 numplanes;
    const nj2k_plane_t *planes;
} nj2k_output_t;

/** The codestream's tile grid and the main header's coding, from nj2k_get_codestream_layout(). */
typedef struct nj2k_codestream_layout {
    NJ2K_SIZE_T struct_size;        /**< sizeof(nj2k_codestream_layout_t) */
    NJ2K_UINT32 tx0, ty0, tdx, tdy; /**< the tile grid's origin and tile size */
    NJ2K_UINT32 tw, th;             /**< tiles across and down */
    NJ2K_UINT32 numlayers;          /**< layers of the main header's coding style */
    NJ2K_UINT32 min_resolutions;    /**< the fewest resolutions of any component (main header) */
} nj2k_codestream_layout_t;

/**
 * Reads the header of a codestream (or of a JP2/JPH file, for a codec made for NJ2K_CODEC_JP2)
 * from p_size bytes at p_data, as nj2k_read_header() reads one from a stream, whatever the codec
 * read or decoded before: the codec starts a new codestream, and keeps the header model it holds
 * when these bytes start with the same main header (the header is then not parsed again), without
 * a component selection. The bytes are only read during the call. On success, *p_image (the
 * caller's structure) describes the image as nj2k_read_header() gives it, with the components at
 * the reduce factor in effect; p_image->comps and p_image->icc_profile_buf point into the codec and
 * stay valid until the codec reads another header or is destroyed: never pass p_image to
 * nj2k_image_destroy(). p_image may be NULL.
 *
 * The decoder limits that bound the decode (the decoded side, the working memory) are not checked
 * here, so that a host can read a header at reduce 0 and then choose a factor: the image may be
 * larger than any decode the limits allow. nj2k_get_decoded_dimensions() checks them as
 * nj2k_decode_memory() will, without decoding; a host sizes its planes once that call (or its own
 * checks) accepts the components' sizes.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_read_header_memory(nj2k_codec_t *p_codec,
        const NJ2K_BYTE *p_data, NJ2K_SIZE_T p_size, nj2k_image_t *p_image);

/**
 * The tile grid and the main header's coding of the header the codec holds, without allocating
 * (nj2k_get_cstr_info() gives more, from the heap).
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_get_codestream_layout(nj2k_codec_t *p_codec,
        nj2k_codestream_layout_t *p_layout);

/**
 * Decodes the codestream (or JP2/JPH file) in p_size bytes at p_data into the caller's planes:
 * reads its header first unless the codec holds the header of the same bytes (as
 * nj2k_read_header_memory() decides), then decodes as nj2k_decode() does and writes each decoded
 * component, converted to p_output->sample_type, to the planes that take it. The planes must hold
 * each component's w x h samples at the reduce factor in effect (nj2k_read_header_memory()
 * describes them); a component no plane takes is decoded all the same, unless a component selection
 * leaves it out (a plane for a component left out is refused, NJ2K_E_OUTPUT). A JP2 file whose
 * palette or channel definitions would change the components is refused (NJ2K_E_UNSUPPORTED;
 * nj2k_decode() applies them) unless the decoder parameters ignore them or components are
 * selected (nj2k_decode() then leaves them as the codestream has them). A JP2 file's codestream
 * ends where its codestream box ends, when the box gives its length (DP-15). The decoder limits
 * are checked from the headers before anything is decoded, and a tile with its own coding again
 * before that tile (NJ2K_E_LIMIT). Success is NJ2K_OK, or
 * NJ2K_TRUNCATED when the bytes end before the codestream does (the planes then hold what arrived,
 * as nj2k_decode() gives it); on failure the planes may have been partly written.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decode_memory(nj2k_codec_t *p_codec,
        const NJ2K_BYTE *p_data, NJ2K_SIZE_T p_size, const nj2k_output_t *p_output);

/**
 * How much memory the codec keeps between decodes (its working memory, Tier-1 and wavelet
 * buffers, a tile's samples): at most bytes, the rest goes back to the allocator after each
 * decode. The default is 8 MiB; 0 keeps nothing. Applies from the next decode.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_retained_bytes(nj2k_codec_t *p_codec,
        NJ2K_SIZE_T bytes);

/*
 * Threads in one image (neo-j2k's own; nj2k_codec_set_threads() stays false: a host lends the codec
 * its executor with nj2k_decoder_set_executor() or nj2k_encoder_set_executor() instead).
 *
 * The library starts no thread of its own and keeps no pool: a host that wants a large image
 * decoded on several cores lends the codec its executor, so every decoder of the process shares the
 * host's one scheduler and nothing oversubscribes the cores, however many decoders run at once.
 * Without an executor everything runs on the calling thread, as before. A decoder calls it only for
 * a tile whose largest component has 2^18 samples or more (512 x 512), and to clear an image plane of
 * 4 MiB or more that the first tile written into it does not cover (a multi-tile image). An encoder
 * calls it from 2^16 samples (256 x 256). Smaller work stays on the calling thread.
 *
 * Busy or idle is the host's decision: lend the executor only when the host has idle cores, for
 * example when its decode queue holds fewer jobs than it has workers, and lend nothing when the queue
 * is backed up. Never lend it to every decode on a saturated pool: splitting a decode
 * there gains nothing and costs about 3 % more CPU (doc/bench.md, stage 5).
 *
 * A codec decodes the same samples, and gives the same status and the same messages in the same order,
 * whatever the executor and its concurrency.
 */

/** One job of a parallel_for: index runs from 0 to count - 1. */
typedef void (*nj2k_job_fn)(void *job_ctx, NJ2K_UINT32 index);

/**
 * A host's executor. parallel_for(user, count, job, job_ctx) runs job(job_ctx, i) once for every
 * i < count, on any threads in any order (the calling thread included), and returns when every one
 * has returned. Memory order: what the caller wrote before the call is seen by every job, and what
 * every job wrote is seen by the caller once the call returns (each job's run happens before the
 * return, as a mutex or a release/acquire pair gives it). Jobs never wait for one another and never
 * call into the codec or the executor, so an executor whose waiting caller runs jobs itself never
 * deadlocks, even with every worker busy. concurrency is how many jobs the host would run at once
 * (the calling thread included): the codec cuts each stage into about that many parts (up to 64).
 * Set struct_size to sizeof(nj2k_executor_t).
 */
typedef struct nj2k_executor {
    NJ2K_SIZE_T struct_size;
    NJ2K_UINT32 concurrency;
    void (*parallel_for)(void *user, NJ2K_UINT32 count, nj2k_job_fn job, void *job_ctx);
    void *user;
} nj2k_executor_t;

/**
 * Lends the codec an executor (copied; its user pointer must stay valid while the codec decodes),
 * or takes it back with NULL. A concurrency below 2 runs everything on the calling thread. The codec
 * calls parallel_for only from the thread that decodes, inside nj2k_decode(), nj2k_decode_memory()
 * and the calls that decode through them. Applies from the next decode. The caller's planes of
 * nj2k_decode_memory() are then written from several threads: a decode into planes that share bytes
 * (a row within another row's reach, or two planes over the same bytes) runs on the calling thread,
 * whose fixed order decides what such bytes hold, as without an executor.
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_decoder_set_executor(nj2k_codec_t *p_codec,
        const nj2k_executor_t *executor);

/**
 * Lends an encoder the same host executor, or takes it back with NULL. Images from 256 x 256 use it for
 * independent component wavelets and Tier-1 code-blocks; small images remain on the calling thread.
 * The produced codestream bytes and diagnostic order do not depend on concurrency. The executor's
 * user pointer must remain valid through nj2k_encode().
 */
NJ2K_API NJ2K_BOOL NJ2K_CALLCONV nj2k_encoder_set_executor(nj2k_codec_t *p_codec,
        const nj2k_executor_t *executor);


#ifdef __cplusplus
}
#endif

#endif /* NEOJ2K_H */
