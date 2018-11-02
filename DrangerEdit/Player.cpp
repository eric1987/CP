#include "Player.h"


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

#define USE_SDL 1
#define USE_QT 0

#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioDeviceInfo>
#include <QtCore/QString>
#include <QtGui/QImage>

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
using namespace std::chrono;
using namespace std;

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

chrono::steady_clock::time_point t;

struct FrameInfo
{
	AVFrame *frame;
	double pts;
};
queue<FrameInfo> qFrameInfo;
queue<AVFrame *> qFrame;
mutex cf_mutex;
condition_variable cf_conV;
bool f_pushed = false;
int f_num = 0;

uchar *med_buffer;
unsigned char *out_buffer;
QIODevice *out;
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
AVPacket flush_pkt;
int eof = 0;
int nomorepkt = 0;

AVFormatContext *fmt_ctx;
AVCodecContext *vctx, *actx;
int video_index = -1;
int audio_index = -1;

double frame_last_pts;
double frame_last_delay;
double video_current_pts;
int64_t video_current_pts_time;
double frame_timer;
void SDLRending(AVFrame *disframe);

double video_clock;	//
					// int64_t audio_clock;	//
					// int64_t frame_timer;
					// int64_t delay;
					// int64_t actual_delay;

bool paIsPushed()
{
	return pa_pushed;
}

bool pvIsPushed()
{
	return pv_pushed;
}

bool fIsPushed()
{
	return f_pushed;
}

void envpkt(queue<AVPacket *> &q, const AVPacket *pkt)
{
	AVPacket *p = av_packet_clone(pkt);
	lock_guard<mutex> ulock(pv_mutex);
	q.push(p);
	pv_size += p->size;
	pv_pushed = true;
	pv_conV.notify_one();
}

void enapkt(queue<AVPacket *> &q, const AVPacket *pkt)
{
	AVPacket *p = av_packet_clone(pkt);
	lock_guard<mutex> ulock(pa_mutex);
	q.push(p);
	pa_size += p->size;
	pa_pushed = true;
	pa_conV.notify_one();
}

int devpkt(queue<AVPacket *> &q, AVPacket *pkt)
{
	unique_lock<mutex> ulock(pv_mutex);
	pv_conV.wait(ulock, bind(pvIsPushed));
	if (q.size() > 0)
	{
		AVPacket *packet = q.front();
		av_packet_ref(pkt, packet);
		q.pop();
		pv_size -= packet->size;
		av_packet_unref(packet);
		return 0;
	}
	else
	{
		return -1;
	}
}

int deapkt(queue<AVPacket *> &q, AVPacket *pkt)
{
	unique_lock<mutex> ulock(pa_mutex);
	pa_conV.wait(ulock, bind(paIsPushed));
	if (q.size() > 0)
	{
		AVPacket *packet = q.front();
		av_packet_ref(pkt, packet);
		q.pop();
		pa_size -= packet->size;
		av_packet_unref(packet);
		return 0;
	}
	else
	{
		return -1;
	}
}

//int depkt(queue<AVPacket *> &q, AVPacket *pkt, mutex &m, bool &b_fun, int &size)
//{
//	unique_lock<mutex> ulock(m);
//	pv_conV.wait(ulock, bind(b_fun));
//	if (q.size() > 0)
//	{
//		AVPacket *packet = q.front();
//		av_packet_ref(pkt, packet);
//		q.pop();
//		size -= packet->size;
//		av_packet_unref(packet);
//		return 0;
//	}
//	else
//	{
//		return -1;
//	}
//}



int clear_qpkt(queue<AVPacket *> &q, mutex &m, int &size, bool &b_pushed)
{
	lock_guard<mutex> ulock(m);
	for (int i = 0; i<q.size(); i++)
	{
		AVPacket *pkt = q.front();
		q.pop();
		av_packet_unref(pkt);
	}
	size = 0;
	b_pushed = false;
	return 0;
}

void enframe(queue<AVFrame *> &q, const AVFrame *frame)
{
	AVFrame *f = av_frame_clone(frame);

	lock_guard<mutex> lockGuard(cf_mutex);
	q.push(f);
	f_num++;
	f_pushed = true;
	cf_conV.notify_one();
}

int deframe(queue<AVFrame *> &q, AVFrame *frame)
{
	unique_lock<mutex> ulock(cf_mutex);
	cf_conV.wait(ulock, bind(fIsPushed));
	if (q.size() > 0)
	{
		AVFrame *f = q.front();
		av_frame_ref(frame, f);
		q.pop();
		f_num--;
		av_frame_unref(f);
		return 0;
	}
	else
	{
		return -1;
	}
}

int clear_qframe(queue<FrameInfo> &q, mutex &m, int &size, bool &b_pushed)
{
	lock_guard<mutex> ulock(m);
	for (int i = 0; i < q.size(); i++)
	{
		FrameInfo frameinfo = q.front();
		q.pop();
		av_frame_unref(frameinfo.frame);
	}
	size = 0;
	b_pushed = false;
	return 0;
}

void enframeinfo(queue<FrameInfo> &q, const AVFrame *frame, const double pts)
{
	AVFrame *f = av_frame_clone(frame);
	FrameInfo frameinfo;
	frameinfo.frame = f;
	frameinfo.pts = pts;

	lock_guard<mutex> lockGuard(cf_mutex);
	q.push(frameinfo);
	f_num++;
	f_pushed = true;
	cf_conV.notify_one();
}

int deframeinfo(queue<FrameInfo> &q, AVFrame *frame, double &pts)
{
	unique_lock<mutex> ulock(cf_mutex);
	cf_conV.wait(ulock, bind(fIsPushed));
	if (q.size() > 0)
	{
		FrameInfo f = q.front();
		av_frame_ref(frame, f.frame);
		pts = f.pts;
		q.pop();
		f_num--;
		av_frame_unref(f.frame);
		return 0;
	}
	else
	{
		return -1;
	}
}

double get_ref_clock()
{
	return av_gettime() / 1000000;
}

double get_video_clock()
{
	double delta;

	delta = (av_gettime() - video_current_pts_time) / 1000000.0;
	return video_current_pts + delta;
}

double synchronize_video(AVFrame *frame, double pts)
{
	double frame_delay;

	if (pts != 0) {
		/* if we have pts, set video clock to it */
		video_clock = pts;
	}
	else {
		/* if we aren't given a pts, set it to the clock */
		pts = video_clock;
	}
	/* update the video clock */
	frame_delay = av_q2d(vctx->time_base);
	/* if we are repeating a frame, adjust clock accordingly */
	frame_delay += frame->repeat_pict * (frame_delay * 0.5);
	video_clock += frame_delay;
	return pts;
}

void video_refresh_timer(double pts, AVFrame *frame)
{
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	if (video_index >= 0)
	{
		/*if (qFrameInfo.size() == 0)
		{
		this_thread::sleep_for(chrono::milliseconds(1));
		}*/
		//else 
		{
			video_current_pts = pts;
			video_current_pts_time = av_gettime();
			delay = pts - frame_last_pts; /* the pts from last time */
			if (delay <= 0 || delay >= 1.0)
			{
				/* if incorrect delay, use previous one */
				delay = frame_last_delay;
			}
			/* save for next time */
			frame_last_delay = delay;
			frame_last_pts = pts;

			/* update delay to sync ref_clock */
			ref_clock = get_ref_clock();
			diff = pts - ref_clock;

			/* Skip or repeat the frame. Take delay into account
			FFPlay still doesn't "know if this is the best guess." */
			sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;
			if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
				if (diff <= -sync_threshold) {
					delay = 0;
				}
				else if (diff >= sync_threshold) {
					delay = 2 * delay;
				}
			}

			frame_timer += delay;
			/* computer the REAL delay */
			actual_delay = frame_timer - (av_gettime() / 1000000.0);
			if (actual_delay < 0.010) {
				/* Really it should skip the picture instead */
				actual_delay = 0.010;
			}
			this_thread::sleep_for(chrono::milliseconds((int)(actual_delay * 1000 + 0.5)));

			/* show the picture! */

#if USE_QT
			QImage img((uchar *)out_buffer, vctx->width,
				vctx->height, QImage::Format_RGB32);
			QImage temp = img.copy();
#endif
			
#if USE_SDL
			SDLRending(frame);
#endif
		}
	}
	else {
		this_thread::sleep_for(chrono::milliseconds(100));
	}
}

void parse_thread(AVFormatContext *fmt_ctx)
{
	int ret = -1;
	AVPacket *pkt = av_packet_alloc();
	for (;;)
	{
		if (eof || be_exit)	//end of stream. quit to read any more pkts.
		{
			break;
		}
		if (seek_req)
		{
			seek_req = 0;
			if (audio_index >= 0)
			{
				clear_qpkt(qAPkt, pa_mutex, pa_size, pa_pushed);
				enapkt(qAPkt, &flush_pkt);
			}
			else if (video_index >= 0)
			{
				clear_qpkt(qVPkt, pv_mutex, pv_size, pv_pushed);
				clear_qframe(qFrameInfo, cf_mutex, f_num, f_pushed);
				envpkt(qVPkt, &flush_pkt);
			}
			//int64_t timestamp = seek_pos*av_q2d(fmt_ctx->streams[video_index]->time_base);
			AVRational baseQ = AVRational{ 1, AV_TIME_BASE };
			int64_t timestamp = av_rescale_q(seek_pos, baseQ,
				fmt_ctx->streams[video_index]->time_base);
			seek_pos = 0;
			int flags = AVSEEK_FLAG_ANY;
			av_seek_frame(fmt_ctx, video_index, timestamp, flags);
		}
		if (pv_size + pa_size >= pkt_size)
		{
			this_thread::sleep_for(chrono::milliseconds(10));
			continue;
		}

		ret = av_read_frame(fmt_ctx, pkt);
		if (ret < 0)
		{
			if (ret == AVERROR_EOF)
			{
				eof = 1;
			}
		}

		if (pkt->stream_index == AVMEDIA_TYPE_VIDEO)
		{
			envpkt(qVPkt, pkt);
		}
		else if (pkt->stream_index == AVMEDIA_TYPE_AUDIO)
		{
			enapkt(qAPkt, pkt);
		}
		av_packet_unref(pkt);
	}

	int endofthread = 1;
}

void de_athread(AVCodecContext *av_ctx)
{
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int ret = -1;
	while (true)
	{
		if (be_exit)
		{
			break;
		}
		if (pause)
		{
			this_thread::sleep_for(chrono::milliseconds(10));
			continue;
		}

		ret = deapkt(qAPkt, pkt);
		if (ret < 0)
		{
			if (eof)
			{
				nomorepkt = 1;
				break;
			}
			else
			{
				continue;
			}
		}
		if (pkt->data == flush_pkt.data)
		{
			avcodec_flush_buffers(actx);
			continue;
		}

		ret = avcodec_send_packet(av_ctx, pkt);
		if (ret < 0)
			continue;

		while (pkt->size > 0)
		{
			int len, data_size = MAX_AUDIO_FRAME_SIZE;
			ret = avcodec_receive_frame(av_ctx, frame);
			if (ret < 0)
				continue;

			int outsize = av_samples_get_buffer_size(NULL, av_ctx->channels,
				frame->nb_samples,
				AV_SAMPLE_FMT_S16,
				0);

			//int out_count = (int64_t)wanted_nb_samples * is->audio_tgt.freq / frame->sample_rate + 256;
			len = swr_convert(swr_ctx, &med_buffer, outsize,
				(const uint8_t **)frame->data, frame->nb_samples);

			out->write((char *)med_buffer, outsize);

			pkt->size -= len;
			pkt->data += len;

			if (out->size() > 10000000)
			{
				break;
			}

			av_frame_unref(frame);
			this_thread::sleep_for(chrono::milliseconds(20));
		}
		av_packet_unref(pkt);
	}

	int endofthread = 1;
}

void de_vthread(AVCodecContext *av_ctx)
{
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	int ret = -1;
	double pts = 0;
	while (true)
	{
		if (be_exit)
		{
			break;
		}
		if (f_num > 3 || pause)
		{
			this_thread::sleep_for(chrono::milliseconds(10));
			continue;
		}
		ret = devpkt(qVPkt, pkt);

		if (ret < 0)
		{
			if (eof)
			{
				nomorepkt = 1;
				break;
			}
			else
			{
				continue;
			}
		}

		if (pkt->data == flush_pkt.data)
		{
			avcodec_flush_buffers(vctx);
			seek_log1 = 1;
			seek_log2 = 1;
			continue;
		}

		ret = avcodec_send_packet(av_ctx, pkt);
		if (ret < 0)
			continue;
		ret = avcodec_receive_frame(av_ctx, frame);
		if (ret < 0)
			continue;

		pts = frame->pts;
		pts *= av_q2d(fmt_ctx->streams[video_index]->time_base);

		pts = synchronize_video(frame, pts);

		//enframe(qFrame, frame);
		enframeinfo(qFrameInfo, frame, pts);

		av_frame_unref(frame);
		av_packet_unref(pkt);
		//this_thread::sleep_for(chrono::milliseconds(20));
	}

	int endofthread = 1;
}

void SDLRending(AVFrame *disframe)
{
	SDL_UpdateTexture(texture, &rect, disframe->data[0], disframe->linesize[0]);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, &rect);
	SDL_RenderPresent(renderer);
}

void toImage(AVCodecContext *ctx)
{
	AVFrame *frame = av_frame_alloc();
	AVFrame *disFrame = av_frame_alloc();

	out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size
	(pix_fmt, ctx->width, ctx->height, 1));
	av_image_fill_arrays(disFrame->data, disFrame->linesize, out_buffer,
		pix_fmt, ctx->width, ctx->height, 1);

	SwsContext *sws_ctx = nullptr;
	sws_ctx = sws_getContext(ctx->width, ctx->height, ctx->pix_fmt,
		ctx->width, ctx->height, pix_fmt, SWS_BICUBIC,
		nullptr, nullptr, nullptr);
	int i = 0;
	int ret = -1;
	double pts = 0;
	while (true)
	{
		i++;
		if (be_exit)
		{
			break;
		}
		ret = deframeinfo(qFrameInfo, frame, pts);
		if (ret < 0)
		{
			if (nomorepkt)
			{
				be_exit = 1;
				break;
			}
			continue;
		}
		sws_scale(sws_ctx, frame->data, frame->linesize, 0,
			frame->height, disFrame->data, disFrame->linesize);

		/*QImage img((uchar *)out_buffer, ctx->width,
		ctx->height, QImage::Format_RGB32);
		QString filename = QString("D:/work/Resource/pic/") + QString::number(i) + QString("pic.png");
		QImage temp = img.copy();
		img.save(filename);*/

		video_refresh_timer(pts, disFrame);
		//SDLRending(disFrame);
		av_frame_unref(frame);
		//av_frame_unref(disFrame);
		//this_thread::sleep_for(chrono::milliseconds(30));
	}
}

void SDLConfiguration(AVCodecContext *av_ctx)
{
	window = SDL_CreateWindow("player",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		av_ctx->width, av_ctx->height,
		SDL_WINDOW_OPENGL);

	renderer = SDL_CreateRenderer(window, -1, 0);
	texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
		SDL_TEXTUREACCESS_STREAMING, av_ctx->width, av_ctx->height);
	//SDL_Rect rect;
	rect.x = 0;
	rect.y = 0;
	rect.w = av_ctx->width;
	rect.h = av_ctx->height;
}

//release resource
void quit_play()
{
	clear_qpkt(qVPkt, pv_mutex, pv_size, pv_pushed);
	clear_qpkt(qAPkt, pa_mutex, pa_size, pa_pushed);
	avcodec_close(actx);
	avcodec_close(vctx);
	avformat_close_input(&fmt_ctx);
}

void Pause()
{
	pause = !pause;
}

void SeekPos(double pos)
{
	mutex m;
	t = steady_clock::now();
	lock_guard<mutex> lock(m);
	frame_timer = (double)av_gettime() / 1000000;

	seek_req = 1;
	seek_pos = pos;
}

void Forward()
{
	double pos = video_clock;
	pos += 10;
	pos *= AV_TIME_BASE;
	SeekPos(pos);
}

void Backward()
{
	double pos = video_clock;
	pos -= 10;
	pos *= AV_TIME_BASE;
	SeekPos(pos);
}

void SDLEventHandle()
{
	SDL_Event event;
	while (true)
	{
		SDL_WaitEvent(&event);
		switch (event.type)
		{
		case SDL_KEYDOWN:
			switch (event.key.keysym.sym)
			{
			case SDLK_LEFT:
				Backward();
				break;
			case SDLK_RIGHT:
				Forward();
			default:
				break;
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
			Pause();
			break;
		default:
			break;
		}
	}
}

int play(string url)
{
	int ret = -1;

	avformat_network_init();

	//flush_pkt->data = (uint8_t *)"FLUSH";
	av_init_packet(&flush_pkt);
	flush_pkt.data = (uint8_t *)&flush_pkt;

#if USE_SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		return -1;
	}
#endif

	//char filename[] = "D:/work/Resource/oceans.mp4";
	char *filename = (char *)url.c_str();
	//memcpy(filename, url.c_str(), sizeof(url.c_str()));

	fmt_ctx = nullptr;
	avformat_open_input(&fmt_ctx, filename, nullptr, nullptr);

	avformat_find_stream_info(fmt_ctx, nullptr);

	av_dump_format(fmt_ctx, 0, filename, 0);

	for (int i = 0; i < fmt_ctx->nb_streams; i++)
	{
		if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			video_index = i;
		}
		else if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audio_index = i;
		}
	}

	vctx = nullptr;
	AVCodec *vcodec = nullptr;

	vctx = avcodec_alloc_context3(nullptr);
	if (!vctx)
	{
		return AVERROR(ENOMEM);
	}
	ret = avcodec_parameters_to_context(vctx, fmt_ctx->streams[video_index]->codecpar);
	vctx->pkt_timebase = fmt_ctx->streams[video_index]->time_base;
	vcodec = avcodec_find_decoder(vctx->codec_id);
	avcodec_open2(vctx, vcodec, nullptr);

	actx = nullptr;
	AVCodec *acodec = nullptr;
	if (audio_index >= 0)
	{
		actx = avcodec_alloc_context3(nullptr);
		if (!actx)
		{
			return AVERROR(ENOMEM);
		}
		ret = avcodec_parameters_to_context(actx, fmt_ctx->streams[audio_index]->codecpar);
		actx->pkt_timebase = fmt_ctx->streams[audio_index]->time_base;
		acodec = avcodec_find_decoder(actx->codec_id);
		avcodec_open2(actx, acodec, nullptr);

		//configuration with QtAudioOutput.
		QAudioFormat audio_fmt;
		audio_fmt.setSampleRate(actx->sample_rate);	//change by specific condition
		audio_fmt.setChannelCount(actx->channels);
		audio_fmt.setSampleSize(16);
		audio_fmt.setCodec("audio/pcm");
		audio_fmt.setByteOrder(QAudioFormat::LittleEndian);
		audio_fmt.setSampleType(QAudioFormat::SignedInt);

		QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());

		if (!info.isFormatSupported(audio_fmt)) {
			cout << "raw audio format not supported by backend, cannot play audio." << endl;
			audio_fmt = info.nearestFormat(audio_fmt);
		}

		QAudioOutput *audio = new QAudioOutput(audio_fmt);

		out = audio->start();
		med_buffer = (uint8_t*)malloc(MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);

		swr_ctx = swr_alloc();
		swr_ctx = swr_alloc_set_opts(swr_ctx, actx->channel_layout,
			AV_SAMPLE_FMT_S16,
			actx->sample_rate,
			actx->channels,
			actx->sample_fmt,
			actx->sample_rate,
			0, 0);
		swr_init(swr_ctx);
	}

#if USE_SDL
	SDLConfiguration(vctx);
#endif

	frame_last_delay = 40e-3;
	video_current_pts_time = av_gettime();
	frame_timer = (double)av_gettime() / 1000000.0;


	thread producer(parse_thread, ref(fmt_ctx));
	thread consumer(de_vthread, ref(vctx));
	thread converter(toImage, ref(vctx));
	if (audio_index >= 0)
	{
		thread audio(de_athread, ref(actx));
		SDLEventHandle();
		audio.join();
	}

	producer.join();
	consumer.join();
	converter.join();

	quit_play();
}

Player::Player()
{
}


Player::~Player()
{
}

void Player::playurl()
{
	play("D:/work/Resource/oceans.mp4");
}
