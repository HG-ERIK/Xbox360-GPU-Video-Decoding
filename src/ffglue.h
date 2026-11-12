// Xbox 360 GPU Video Decoding. Copyright (C) 2026 HG-ERIK. GPL-3.0-or-later, see LICENSE.
#ifndef FFGLUE_H
#define FFGLUE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FgSource
{
    void*   opaque;
    int     ( *read )( void* opaque, unsigned char* buf, int size );
    int     ( *seek )( void* opaque, __int64 pos );
    __int64 size;
} FgSource;

typedef struct FgMedia FgMedia;

typedef struct FgInfo
{
    int     hasVideo, hasAudio;
    int     width, height;
    double  fps;
    double  duration;
    int     sampleRate, channels;
    char    videoCodec[32], audioCodec[32], container[32];
    int     threads;
    int     frameThreading;
    int     profile, level, refs;
    double  pixelAspect;
} FgInfo;

typedef struct FgPicture
{
    const unsigned char* plane[3];
    int                  pitch[3];
    int                  width, height;
    double               pts;
    int                  colorspace;
    int                  fullRange;
} FgPicture;

enum { FG_CS_UNKNOWN = 0, FG_CS_BT601 = 1, FG_CS_BT709 = 2 };

typedef struct FgPacket FgPacket;
enum { FG_PACKET_VIDEO = 1, FG_PACKET_AUDIO = 2, FG_PACKET_OTHER = 3, FG_PACKET_SUBTITLE = 4 };

typedef void ( *FgLogFn )( const char* line );
void     fg_init( FgLogFn log );

void     fg_set_decode_threads( int n );

void     fg_set_stereo( int on );

void     fg_set_audio_track( int containerIndex );

FgMedia* fg_open( FgSource* src, const char* name, char* err, int errSize );
void     fg_close( FgMedia* m );
void     fg_get_info( FgMedia* m, FgInfo* info );

int      fg_read_packet( FgMedia* m, FgPacket** pkt );
void     fg_free_packet( FgPacket* pkt );
double   fg_packet_time( FgMedia* m, FgPacket* pkt );

int      fg_packet_has_time( FgPacket* pkt );

int      fg_packet_annexb( FgMedia* m, FgPacket* pkt, unsigned char** out, int* outSize, int withHeaders );

int      fg_decode_video( FgMedia* m, FgPacket* pkt, FgPicture* pic );

int      fg_decode_audio( FgMedia* m, FgPacket* pkt, short* pcm, int pcmBytes, double* pts );

typedef struct FgTrack
{
    int  index;
    int  type;
    int  channels;
    int  text;
    char codec[32], language[16], title[64];
} FgTrack;

int      fg_track_count( FgMedia* m );
void     fg_get_track( FgMedia* m, int n, FgTrack* track );

void     fg_select_subtitle( FgMedia* m, int containerIndex );

int      fg_subtitle( FgMedia* m, FgPacket* pkt, char* text, int size, double* start, double* duration );

int      fg_seek( FgMedia* m, double seconds );

void     fg_set_fast( FgMedia* m, int level );

int      fg_vmx_status( void );
int      fg_vmx_qpel_status( void );

void     fg_profile( double* cabacMs, double* reconMs, double* filterMs, double* waitMs, int reset );

#ifdef __cplusplus
}
#endif

#endif
