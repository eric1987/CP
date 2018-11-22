#pragma once

#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioDeviceInfo>
#include <QtCore/QString>
#include <QtGui/QImage>
#include <QtCore/QTime>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/time.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>

#include <SDL.h>
#include <SDL_thread.h>
}

#define  __SDL__ 1
#undef main
#ifdef __MINGW32__
#undef main /* Prevents SDL from overriding main() */
#endif

#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <iostream>

#include <ctime>
#include <ratio>
#include <chrono>
using namespace std;
using namespace std::chrono;

// compatibility with newer API
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55,28,1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#define SDL_AUDIO_BUFFER_SIZE 1024
#define MAX_AUDIO_FRAME_SIZE 192000
#define FF_INPUT_BUFFER_PADDING_SIZE 32

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define AV_SYNC_THRESHOLD 0.01
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 10
#define AUDIO_DIFF_AVG_NB 20

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define VIDEO_PICTURE_QUEUE_SIZE 1

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

class CP
{
	struct FrameInfo
	{
		AVFrame *frame;
		double pts;
	};

public:
	CP();
	~CP();

	void ParseThread(AVFormatContext *fmt_ctx);
	void VideoThread(AVCodecContext *av_ctx);
	void AudioThread(AVCodecContext *av_ctx);
	void DisplayThread(AVCodecContext *ctx);
	void SDLConfiguration(AVCodecContext *av_ctx);
	int SDLConfigureAudio(AVCodecContext *av_ctx);
	void SDLEventHandle();
	void quit_play();

	void Pause();
	void Stop();
	void SeekPos(double pos);
	void Forward();
	void Backward();
	void NextFrame();
	void LastFrame();	//not working yet
	void HalfPlay();
	void DoublePlay();

	static void AudioCallback(void *userdata, Uint8 *stream, int len);

private:
	bool paIsPushed();
	bool pvIsPushed();
	bool fIsPushed();
	void envpkt(queue<AVPacket *> &q, const AVPacket *pkt);
	void enapkt(queue<AVPacket *> &q, const AVPacket *pkt);
	int devpkt(queue<AVPacket *> &q, AVPacket *pkt);
	int deapkt(queue<AVPacket *> &q, AVPacket *pkt);
	int clear_qpkt(queue<AVPacket *> &q, mutex &m, int &size, bool &b_pushed);
	void enframe(queue<AVFrame *> &q, const AVFrame *frame);
	int deframe(queue<AVFrame *> &q, AVFrame *frame);
	int clear_qframe(queue<FrameInfo> &q, mutex &m, int &size, bool &b_pushed);
	void enframeinfo(queue<FrameInfo> &q, const AVFrame *frame, const double pts);
	int deframeinfo(queue<FrameInfo> &q, AVFrame *frame, double &pts);
	double get_ref_clock();
	double get_video_clock();
	double synchronize_video(AVFrame *frame, double pts);
	void video_refresh_timer(double pts, AVFrame *frame);
	void SDLRending(AVFrame *disframe);
	int audio_decode_frame(uint8_t *audio_buf, int buf_size, double *pts_ptr);
	int synchronize_audio(short *samples, int samples_size, double pts);
	double get_audio_clock();

public:
	AVPacket flush_pkt;
	AVFormatContext *fmt_ctx = nullptr;
	int video_index = -1;
	int audio_index = -1;

public:
	queue<AVPacket *> qVPkt;
	queue<AVPacket *> qAPkt;
	int pv_size = 0;
	int pa_size = 0;
	const int pkt_size = 5 * 256 * 1024 + 5 * 16 * 1024;	//size by memory of pkt queue.

	mutex pv_mutex;
	mutex pa_mutex;
	condition_variable pa_conV;
	condition_variable pv_conV;
	bool pa_pushed = false;
	bool pv_pushed = false;

	queue<FrameInfo> qFrameInfo;
	queue<AVFrame *> qFrame;
	mutex cf_mutex;
	condition_variable cf_conV;
	bool f_pushed = false;
	int f_num = 0;

	unsigned char *med_buffer;
	unsigned char *out_buffer;
	QIODevice *out;
	uint8_t *outa_buffer;
	SwrContext *swr_ctx;

	SDL_Window *window;
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	SDL_Rect rect;

	AVPixelFormat pix_fmt = AV_PIX_FMT_YUV420P;	//AV_PIX_FMT_YUV420P,  AV_PIX_FMT_RGB32

	int be_exit = 0;
	bool pause = false;
	int seek_log1 = 0;
	int seek_log2 = 0;
	int seek_req = 0;
	double seek_pos = 0;
	//AVPacket *flush_pkt = av_packet_alloc();
	
	int eof = 0;
	int nomorepkt = 0;
	
	AVCodecContext *vctx, *actx;
	
	double frame_last_pts;
	double frame_last_delay;
	double video_current_pts;
	int64_t video_current_pts_time;
	double frame_timer;
	double video_clock;

	//int audio_buf_index;
	//int audio_buf_size;
	SDL_AudioDeviceID dev;
	int audio_hw_buf_size;
	uint8_t audio_buf[(MAX_AUDIO_FRAME_SIZE * 3) / 2];
	unsigned int audio_buf_size;
	unsigned int audio_buf_index;
	int audio_pkt_size;
	uint8_t *audio_pkt_data;
	double audio_clock;
	AVStream *audio_st;
	int av_sync_type;
	double audio_diff_cum;
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	int audio_diff_avg_count;
	uint8_t  *audio_chunk;
	uint32_t  audio_len;
	uint8_t  *audio_pos;
	int out_buffer_size;

	
	QTime time;
	int step = 0;
	int flags = 0;
	int seek_drop = 0;	//用于丢弃快进或快退后 仍然会收到的几帧非关键帧数据
	int speed = 1;
};

class PlayCore
{
	enum OPTION
	{
		PAUSE,
		STOP,
		FORWARD,
		BACKWARD,
		NEXTFRAME,
		LASTFRAME,
	};

public:
	PlayCore();
	~PlayCore();

	int runplay(string url);
	void play(string url);
	void control(OPTION option);
	
	CP cp;
	
private:
	
	thread play_thread;
};