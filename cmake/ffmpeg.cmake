# FFmpeg's libavcodec (LGPL-2.1-or-later), for the films' MPEG-2 video and the ATRAC3plus sound: the owner's decision of
# 2026-11-22 (docs/roadmap-and-design-targets.md). Fetched and pinned, and built here by CMake from the files the two
# decoders and the MPEG video parser need (FFmpeg's own build is a configure script and make, which not every machine
# this builds on has). The list below was found from FFmpeg's configure (each component's "select") and its Makefiles'
# object lists; nothing of FFmpeg's is copied into this repository.
#
# Its configuration is written here: portable C (no assembler, no threads, no hardware decoding). Every HAVE_, CONFIG_ and
# ARCH_ name the sources use is defined, 0 unless listed below: they are found by scanning the sources, so a new release
# needs no new list of names, only a look at what should be on.

FetchContent_Declare(ffmpeg
    URL https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
    URL_HASH SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1)
FetchContent_MakeAvailable(ffmpeg)  # (it has no CMakeLists: only unpacked)
set(QILIN_FFMPEG ${ffmpeg_SOURCE_DIR})

set(qilin_ffmpeg_sources
    libavcodec/ac3_parser.c
    libavcodec/adts_parser.c
    libavcodec/allcodecs.c
    libavcodec/atrac.c
    libavcodec/atrac3plus.c
    libavcodec/atrac3plusdec.c
    libavcodec/atrac3plusdsp.c
    libavcodec/avcodec.c
    libavcodec/avdct.c
    libavcodec/avfft.c
    libavcodec/bitstream.c
    libavcodec/bitstream_filters.c
    libavcodec/blockdsp.c
    libavcodec/bsf.c
    libavcodec/codec_desc.c
    libavcodec/codec_par.c
    libavcodec/d3d11va.c
    libavcodec/decode.c
    libavcodec/dirac.c
    libavcodec/dv_profile.c
    libavcodec/encode.c
    libavcodec/error_resilience.c
    libavcodec/get_buffer.c
    libavcodec/h264chroma.c
    libavcodec/hpeldsp.c
    libavcodec/idctdsp.c
    libavcodec/imgconvert.c
    libavcodec/jni.c
    libavcodec/jrevdct.c
    libavcodec/lcevcdec.c
    libavcodec/mathtables.c
    libavcodec/me_cmp.c
    libavcodec/mediacodec.c
    libavcodec/mpeg12.c
    libavcodec/mpeg12data.c
    libavcodec/mpeg12dec.c
    libavcodec/mpeg12framerate.c
    libavcodec/mpeg_er.c
    libavcodec/mpegpicture.c
    libavcodec/mpegutils.c
    libavcodec/mpegvideo.c
    libavcodec/mpegvideo_dec.c
    libavcodec/mpegvideo_motion.c
    libavcodec/mpegvideo_parser.c
    libavcodec/mpegvideodata.c
    libavcodec/options.c
    libavcodec/packet.c
    libavcodec/parser.c
    libavcodec/parsers.c
    libavcodec/profiles.c
    libavcodec/qsv_api.c
    libavcodec/raw.c
    libavcodec/refstruct.c
    libavcodec/rl.c
    libavcodec/simple_idct.c
    libavcodec/sinewin.c
    libavcodec/threadprogress.c
    libavcodec/to_upper4.c
    libavcodec/utils.c
    libavcodec/version.c
    libavcodec/videodsp.c
    libavcodec/vlc.c
    libavcodec/vorbis_parser.c
    libavcodec/xiph.c
    libavutil/adler32.c
    libavutil/aes.c
    libavutil/aes_ctr.c
    libavutil/ambient_viewing_environment.c
    libavutil/audio_fifo.c
    libavutil/avsscanf.c
    libavutil/avstring.c
    libavutil/base64.c
    libavutil/blowfish.c
    libavutil/bprint.c
    libavutil/buffer.c
    libavutil/camellia.c
    libavutil/cast5.c
    libavutil/channel_layout.c
    libavutil/cpu.c
    libavutil/crc.c
    libavutil/csp.c
    libavutil/des.c
    libavutil/detection_bbox.c
    libavutil/dict.c
    libavutil/display.c
    libavutil/dovi_meta.c
    libavutil/downmix_info.c
    libavutil/encryption_info.c
    libavutil/error.c
    libavutil/eval.c
    libavutil/executor.c
    libavutil/fifo.c
    libavutil/file.c
    libavutil/file_open.c
    libavutil/film_grain_params.c
    libavutil/fixed_dsp.c
    libavutil/float_dsp.c
    libavutil/frame.c
    libavutil/hash.c
    libavutil/hdr_dynamic_metadata.c
    libavutil/hdr_dynamic_vivid_metadata.c
    libavutil/hmac.c
    libavutil/hwcontext.c
    libavutil/iamf.c
    libavutil/imgutils.c
    libavutil/integer.c
    libavutil/intmath.c
    libavutil/lfg.c
    libavutil/lls.c
    libavutil/log.c
    libavutil/log2_tab.c
    libavutil/lzo.c
    libavutil/mastering_display_metadata.c
    libavutil/mathematics.c
    libavutil/md5.c
    libavutil/mem.c
    libavutil/murmur3.c
    libavutil/opt.c
    libavutil/parseutils.c
    libavutil/pixdesc.c
    libavutil/pixelutils.c
    libavutil/random_seed.c
    libavutil/rational.c
    libavutil/rc4.c
    libavutil/reverse.c
    libavutil/ripemd.c
    libavutil/samplefmt.c
    libavutil/sha.c
    libavutil/sha512.c
    libavutil/slicethread.c
    libavutil/spherical.c
    libavutil/stereo3d.c
    libavutil/tea.c
    libavutil/threadmessage.c
    libavutil/time.c
    libavutil/timecode.c
    libavutil/timestamp.c
    libavutil/tree.c
    libavutil/twofish.c
    libavutil/tx.c
    libavutil/tx_double.c
    libavutil/tx_float.c
    libavutil/tx_int32.c
    libavutil/utils.c
    libavutil/uuid.c
    libavutil/version.c
    libavutil/video_enc_params.c
    libavutil/video_hint.c
    libavutil/xga_font_data.c
    libavutil/xtea.c)
list(TRANSFORM qilin_ffmpeg_sources PREPEND ${QILIN_FFMPEG}/)

# What is on.
set(qilin_ffmpeg_on
    CONFIG_MPEG2VIDEO_DECODER CONFIG_ATRAC3P_DECODER CONFIG_MPEGVIDEO_PARSER
    CONFIG_BLOCKDSP CONFIG_ERROR_RESILIENCE CONFIG_H264CHROMA CONFIG_HPELDSP CONFIG_IDCTDSP CONFIG_ME_CMP CONFIG_MPEG_ER CONFIG_MPEGVIDEO CONFIG_MPEGVIDEODEC CONFIG_SINEWIN CONFIG_VIDEODSP
    CONFIG_AVCODEC CONFIG_AVUTIL CONFIG_SAFE_BITSTREAM_READER CONFIG_NULL_BSF
    HAVE_ATAN2F HAVE_ATANF HAVE_CBRT HAVE_CBRTF HAVE_COPYSIGN HAVE_COSF HAVE_ERF HAVE_EXP2 HAVE_EXP2F HAVE_EXPF HAVE_HYPOT
    HAVE_ISFINITE HAVE_ISINF HAVE_ISNAN HAVE_LDEXPF HAVE_LLRINT HAVE_LLRINTF HAVE_LOG10F HAVE_LOG2 HAVE_LOG2F HAVE_LRINT
    HAVE_LRINTF HAVE_POWF HAVE_RINT HAVE_ROUND HAVE_ROUNDF HAVE_SINF HAVE_TRUNC HAVE_TRUNCF
    HAVE_FAST_64BIT HAVE_LOCAL_ALIGNED HAVE_SIMD_ALIGN_16 HAVE_GETENV)
if(WIN32)
    list(APPEND qilin_ffmpeg_on HAVE_ALIGNED_MALLOC HAVE_IO_H HAVE_WINDOWS_H HAVE_GETPROCESSAFFINITYMASK HAVE_GETSYSTEMTIMEASFILETIME
         HAVE_GETSTDHANDLE HAVE_SETCONSOLETEXTATTRIBUTE HAVE_DOS_PATHS HAVE_SLEEP)
else()
    list(APPEND qilin_ffmpeg_on HAVE_UNISTD_H HAVE_ISATTY HAVE_SYSCONF HAVE_CLOCK_GETTIME HAVE_GETTIMEOFDAY HAVE_GMTIME_R HAVE_LOCALTIME_R
         HAVE_MKSTEMP HAVE_POSIX_MEMALIGN HAVE_NANOSLEEP HAVE_USLEEP HAVE_SYS_PARAM_H HAVE_FCNTL)
endif()
# Names the sources define for themselves, or pass on the command line.
set(qilin_ffmpeg_own CONFIG_COUNT HAVE_TILEMAP HAVE_COMPRESSED_TILEMAP HAVE_7REGS HAVE_AV_CONFIG_H)

set(qilin_ffmpeg_gen ${CMAKE_BINARY_DIR}/ffmpeg-config)
if(NOT EXISTS ${qilin_ffmpeg_gen}/config.h)
    file(GLOB qilin_ffmpeg_headers ${QILIN_FFMPEG}/libavutil/*.h ${QILIN_FFMPEG}/libavcodec/*.h)
    set(names "")
    foreach(f IN LISTS qilin_ffmpeg_sources qilin_ffmpeg_headers)
        file(STRINGS ${f} lines REGEX "(HAVE|CONFIG|ARCH)_[A-Z0-9_]+")
        string(REGEX MATCHALL "(HAVE|CONFIG|ARCH)_[A-Z0-9_]+" found "${lines}")
        list(APPEND names ${found})
    endforeach()
    list(REMOVE_DUPLICATES names)
    list(SORT names)
    list(REMOVE_ITEM names ${qilin_ffmpeg_own})
    set(text "/* Written by cmake/ffmpeg.cmake. */\n#ifndef QILIN_FFMPEG_CONFIG_H\n#define QILIN_FFMPEG_CONFIG_H\n")
    string(APPEND text "#define FFMPEG_CONFIGURATION \"qilin\"\n#define FFMPEG_LICENSE \"LGPL version 2.1 or later\"\n")
    string(APPEND text "#define CONFIG_THIS_YEAR 2025\n#define FFMPEG_DATADIR \"\"\n#define AVCONV_DATADIR \"\"\n#define CC_IDENT \"\"\n")
    string(APPEND text "#define OS_NAME qilin\n#define EXTERN_PREFIX \"\"\n#define EXTERN_ASM\n#define BUILDSUF \"\"\n#define SLIBSUF \"\"\n#define SWS_MAX_FILTER_SIZE 256\n")
    string(APPEND text "#if defined(_MSC_VER)\n#define av_restrict __restrict\n#else\n#define av_restrict restrict\n#endif\n")
    foreach(n IN LISTS names)
        if(n IN_LIST qilin_ffmpeg_on)
            string(APPEND text "#define ${n} 1\n")
        else()
            string(APPEND text "#define ${n} 0\n")
        endif()
    endforeach()
    string(APPEND text "#endif\n")
    file(WRITE ${qilin_ffmpeg_gen}/config.h "${text}")
    file(WRITE ${qilin_ffmpeg_gen}/config_components.h "#include \"config.h\"\n")
    file(WRITE ${qilin_ffmpeg_gen}/libavutil/avconfig.h "#ifndef AVUTIL_AVCONFIG_H\n#define AVUTIL_AVCONFIG_H\n#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 0\n#endif\n")
    file(WRITE ${qilin_ffmpeg_gen}/libavutil/ffversion.h "#ifndef AVUTIL_FFVERSION_H\n#define AVUTIL_FFVERSION_H\n#define FFMPEG_VERSION \"7.1.1\"\n#endif\n")
    file(WRITE ${qilin_ffmpeg_gen}/libavcodec/codec_list.c "static const FFCodec * const codec_list[] = {\n    &ff_mpeg2video_decoder,\n    &ff_atrac3p_decoder,\n    NULL };\n")
    file(WRITE ${qilin_ffmpeg_gen}/libavcodec/parser_list.c "static const AVCodecParser * const parser_list[] = {\n    &ff_mpegvideo_parser,\n    NULL };\n")
    file(WRITE ${qilin_ffmpeg_gen}/libavcodec/bsf_list.c "static const FFBitStreamFilter * const bitstream_filters[] = {\n    &ff_null_bsf,\n    NULL };\n")
endif()

add_library(qilin_ffmpeg STATIC ${qilin_ffmpeg_sources} ${QILIN_FFMPEG}/libavcodec/bsf/null.c)
target_include_directories(qilin_ffmpeg PRIVATE ${qilin_ffmpeg_gen} ${QILIN_FFMPEG})
set_source_files_properties(${QILIN_FFMPEG}/libavcodec/bsf/null.c PROPERTIES INCLUDE_DIRECTORIES ${QILIN_FFMPEG}/libavcodec)
target_include_directories(qilin_ffmpeg SYSTEM INTERFACE ${qilin_ffmpeg_gen} ${QILIN_FFMPEG})
target_compile_definitions(qilin_ffmpeg PRIVATE HAVE_AV_CONFIG_H=1 _ISOC99_SOURCE _FILE_OFFSET_BITS=64 _LARGEFILE_SOURCE _POSIX_C_SOURCE=200112 _XOPEN_SOURCE=600)
set_target_properties(qilin_ffmpeg PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON POSITION_INDEPENDENT_CODE ON)
# Not ours to warn about.
if(MSVC)
    target_compile_options(qilin_ffmpeg PRIVATE /w /utf-8 /experimental:c11atomics)
    target_compile_definitions(qilin_ffmpeg PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
else()
    target_compile_options(qilin_ffmpeg PRIVATE -w -fno-math-errno -fno-signed-zeros -fno-tree-vectorize)
endif()
if(NOT WIN32)
    target_link_libraries(qilin_ffmpeg PUBLIC m)
endif()
