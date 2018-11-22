#include "CP.h"

CP::CP()
{
}


CP::~CP()
{
}

void CP::ParseThread(AVFormatContext *fmt_ctx)
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
			
			if (audio_index >= 0)
			{
				clear_qpkt(qAPkt, pa_mutex, pa_size, pa_pushed);
				enapkt(qAPkt, &flush_pkt);
				avcodec_flush_buffers(actx);
			}
			if (video_index >= 0)
			{
				clear_qpkt(qVPkt, pv_mutex, pv_size, pv_pushed);
				//clear_qframe(qFrameInfo, cf_mutex, f_num, f_pushed);
				envpkt(qVPkt, &flush_pkt);
				//avcodec_flush_buffers(vctx);
			}
			//int64_t timestamp = seek_pos*av_q2d(fmt_ctx->streams[video_index]->time_base);
			AVRational baseQ = AVRational{ 1, AV_TIME_BASE };
			int64_t timestamp = av_rescale_q(seek_pos, baseQ,
				fmt_ctx->streams[video_index]->time_base);
			seek_pos = 0;
			//int flags = AVSEEK_FLAG_ANY;
			
			av_seek_frame(fmt_ctx, video_index, timestamp, flags);
			seek_req = 0;
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
}

void CP::VideoThread(AVCodecContext *av_ctx)
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
		if (f_num > 3)
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

// 		if (pkt->data == flush_pkt.data)
// 		{
// 			avcodec_flush_buffers(vctx);
// 			continue;
// 		}

		ret = avcodec_send_packet(av_ctx, pkt);
		if (ret < 0)
			continue;
		ret = avcodec_receive_frame(av_ctx, frame);
		if (ret < 0)
			continue;

		if (seek_drop)
		{
			if (frame->key_frame)
			{
				seek_drop = 0;
			}
			else
			{
				av_frame_unref(frame);
				continue;
			}
		}


		pts = frame->pts;
		pts *= av_q2d(fmt_ctx->streams[video_index]->time_base);

		pts = synchronize_video(frame, pts);

		//enframe(qFrame, frame);
		enframeinfo(qFrameInfo, frame, pts);

		av_frame_unref(frame);
		av_packet_unref(pkt);
		//this_thread::sleep_for(chrono::milliseconds(20));
	}
}

void CP::AudioThread(AVCodecContext *av_ctx)
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
}

void CP::DisplayThread(AVCodecContext *ctx)
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
		if (pause)
		{
			continue;
			this_thread::sleep_for(chrono::milliseconds(10));
		}
		if (step)
		{
			pause = 1;
			step = 0;
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
		av_frame_unref(frame);
	}
}

void CP::SDLConfiguration(AVCodecContext *av_ctx)
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

int CP::SDLConfigureAudio(AVCodecContext *actx)
{
	uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
	//nb_samples: AAC-1024 MP3-1152
	int out_nb_samples = actx->frame_size;
	AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
	int out_sample_rate = actx->sample_rate;
	int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
	//Out Buffer Size
	int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);

	outa_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);

	SDL_AudioSpec wantedSpec;
	wantedSpec.freq = out_sample_rate;
	wantedSpec.format = AUDIO_S16SYS;
	wantedSpec.channels = out_channels;
	wantedSpec.silence = 0;
	wantedSpec.samples = out_nb_samples;
	wantedSpec.callback = CP::AudioCallback;
	wantedSpec.userdata = this;

	if (SDL_OpenAudio(&wantedSpec, NULL) < 0) {
		printf("can't open audio.\n");
		return -1;
	}

	int64_t in_channel_layout = av_get_default_channel_layout(actx->channels);

	swr_ctx = swr_alloc();
	swr_ctx = swr_alloc_set_opts(swr_ctx, out_channel_layout, out_sample_fmt, out_sample_rate,
		in_channel_layout, actx->sample_fmt, actx->sample_rate, 0, NULL);
	swr_init(swr_ctx);

	SDL_PauseAudio(0);
}

void CP::SDLEventHandle()
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
			case SDLK_UP:
				NextFrame();
			case SDLK_DOWN:
				;
			case SDLK_s:
				HalfPlay();
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

void CP::quit_play()
{
	clear_qpkt(qVPkt, pv_mutex, pv_size, pv_pushed);
	clear_qpkt(qAPkt, pa_mutex, pa_size, pa_pushed);
	avcodec_close(actx);
	avcodec_close(vctx);
	avformat_close_input(&fmt_ctx);
}

bool CP::paIsPushed()
{
	return pa_pushed;
}

bool CP::pvIsPushed()
{
	return pv_pushed;
}

bool CP::fIsPushed()
{
	return f_pushed;
}

void CP::envpkt(queue<AVPacket *> &q, const AVPacket *pkt)
{
	AVPacket *p = av_packet_clone(pkt);
	lock_guard<mutex> ulock(pv_mutex);
	q.push(p);
	pv_size += p->size;
	pv_pushed = true;
	pv_conV.notify_one();
}

void CP::enapkt(queue<AVPacket *> &q, const AVPacket *pkt)
{
	AVPacket *p = av_packet_clone(pkt);
	lock_guard<mutex> ulock(pa_mutex);
	q.push(p);
	pa_size += p->size;
	pa_pushed = true;
	pa_conV.notify_one();
}

int CP::devpkt(queue<AVPacket *> &q, AVPacket *pkt)
{
	unique_lock<mutex> ulock(pv_mutex);
	pv_conV.wait(ulock, bind(&CP::pvIsPushed, this));
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

int CP::deapkt(queue<AVPacket *> &q, AVPacket *pkt)
{
	unique_lock<mutex> ulock(pa_mutex);
	pa_conV.wait(ulock, bind(&CP::paIsPushed, this));
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

int CP::clear_qpkt(queue<AVPacket *> &q, mutex &m, int &size, bool &b_pushed)
{
	lock_guard<mutex> ulock(m);
	for (int i = 0; i < q.size(); i++)
	{
		AVPacket *pkt = q.front();
		q.pop();
		av_packet_unref(pkt);
	}
	size = 0;
	b_pushed = false;
	return 0;
}

void CP::enframe(queue<AVFrame *> &q, const AVFrame *frame)
{
	AVFrame *f = av_frame_clone(frame);

	lock_guard<mutex> lockGuard(cf_mutex);
	q.push(f);
	f_num++;
	f_pushed = true;
	cf_conV.notify_one();
}

int CP::deframe(queue<AVFrame *> &q, AVFrame *frame)
{
	unique_lock<mutex> ulock(cf_mutex);
	cf_conV.wait(ulock, bind(&CP::fIsPushed, this));
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

int CP::clear_qframe(queue<FrameInfo> &q, mutex &m, int &size, bool &b_pushed)
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

void CP::enframeinfo(queue<FrameInfo> &q, const AVFrame *frame, const double pts)
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

int CP::deframeinfo(queue<FrameInfo> &q, AVFrame *frame, double &pts)
{
	unique_lock<mutex> ulock(cf_mutex);
	cf_conV.wait(ulock, bind(&CP::fIsPushed, this));
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

double CP::get_ref_clock()
{
	return speed * av_gettime() / 1000000;
}

double CP::get_video_clock()
{
	double delta;

	delta = (av_gettime() - video_current_pts_time) / 1000000.0;
	return video_current_pts + delta;
}

double CP::synchronize_video(AVFrame *frame, double pts)
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

void CP::video_refresh_timer(double pts, AVFrame *frame)
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


			SDLRending(frame);
		}
	}
	else {
		this_thread::sleep_for(chrono::milliseconds(100));
	}
}

void CP::SDLRending(AVFrame *disframe)
{
	SDL_UpdateTexture(texture, &rect, disframe->data[0], disframe->linesize[0]);
	SDL_RenderClear(renderer);
	SDL_RenderCopy(renderer, texture, NULL, &rect);
	SDL_RenderPresent(renderer);
}

int CP::audio_decode_frame(uint8_t *audio_buf, int buf_size, double *pts_ptr)
{
	int len1, data_size = 0;
	AVPacket *pkt = av_packet_alloc();
	AVFrame *frame = av_frame_alloc();
	double pts;
	int n;
	int ret = -1;

	for (;;) {
		while (audio_pkt_size > 0) {
			ret = avcodec_send_packet(actx, pkt);

			ret = avcodec_receive_frame(actx, frame);
			//len1 = avcodec_decode_audio4(actx, frame, &got_frame, pkt);
			len1 = frame->pkt_size;
			if (len1 < 0) {
				/* if error, skip frame */
				audio_pkt_size = 0;
				break;
			}
			//data_size = 0;
			data_size = 4096;
			if (ret >= 0) {
				/*data_size = av_samples_get_buffer_size(NULL,
					actx->channels,
					frame->nb_samples,
					actx->sample_fmt,
					1);*/
				
				assert(data_size <= buf_size);
				//memcpy(audio_buf, frame->data[0], data_size);
				
				swr_convert(swr_ctx, &outa_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t **)frame->data, frame->nb_samples);
				//memcpy(audio_buf, med_buffer, data_size);
			}
			audio_pkt_data += data_size;
			audio_pkt_size -= data_size;
			if (data_size <= 0) {
				/* No data yet, get more frames */
				continue;
			}
			pts = audio_clock;
			*pts_ptr = pts;
			n = 2 * actx->channels;
			audio_clock += (double)data_size /
				(double)(n * actx->sample_rate);
			/* We have data, return it and come back for more later */
			return data_size;
		}
		if (pkt->data)
			av_packet_unref(pkt);

		if (be_exit) {
			return -1;
		}
		/* next packet */
		if (deapkt(qAPkt, pkt) < 0) {
			return -1;
		}
		if (pkt->data == flush_pkt.data) {
			avcodec_flush_buffers(actx);
			continue;
		}
		audio_pkt_data = pkt->data;
		audio_pkt_size = pkt->size;
		/* if update, update the audio clock w/pts */
		if (pkt->pts != AV_NOPTS_VALUE) {
			audio_clock = av_q2d(audio_st->time_base)*pkt->pts;
		}
	}
}

int CP::synchronize_audio(short *samples, int samples_size, double pts)
{
	int n;
	double ref_clock;

	n = 2 * actx->channels;

	double diff, avg_diff;
	int wanted_size, min_size, max_size /*, nb_samples */;

	ref_clock = av_gettime() / 1000000.0;
	diff = get_audio_clock() - ref_clock;

	if (diff < AV_NOSYNC_THRESHOLD) {
		// accumulate the diffs
		audio_diff_cum = diff + audio_diff_avg_coef
			* audio_diff_cum;
		if (audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
			audio_diff_avg_count++;
		}
		else {
			avg_diff = audio_diff_cum * (1.0 - audio_diff_avg_coef);
			if (fabs(avg_diff) >= audio_diff_threshold) {
				wanted_size = samples_size + ((int)(diff * actx->sample_rate) * n);
				min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
				max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);
				if (wanted_size < min_size) {
					wanted_size = min_size;
				}
				else if (wanted_size > max_size) {
					wanted_size = max_size;
				}
				if (wanted_size < samples_size) {
					/* remove samples */
					samples_size = wanted_size;
				}
				else if (wanted_size > samples_size) {
					uint8_t *samples_end, *q;
					int nb;

					/* add samples by copying final sample*/
					nb = (samples_size - wanted_size);
					samples_end = (uint8_t *)samples + samples_size - n;
					q = samples_end + n;
					while (nb > 0) {
						memcpy(q, samples_end, n);
						q += n;
						nb -= n;
					}
					samples_size = wanted_size;
				}
			}
		}
	}
	else {
		/* difference is TOO big; reset diff stuff */
		audio_diff_avg_count = 0;
		audio_diff_cum = 0;
	}
	return samples_size;
}

double CP::get_audio_clock()
{
	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = audio_clock; /* maintained in the audio thread */
	hw_buf_size = audio_buf_size - audio_buf_index;
	bytes_per_sec = 0;
	n = actx->channels * 2;
	if (audio_st) {
		bytes_per_sec = actx->sample_rate * n;
	}
	if (bytes_per_sec) {
		pts -= (double)hw_buf_size / bytes_per_sec;
	}
	return pts;
}

void CP::Pause()
{
	pause = !pause;
	frame_timer = (double)av_gettime() / 1000000;
}

void CP::Stop()
{
	be_exit = 1;
}

void CP::SeekPos(double pos)
{
	mutex m;
	lock_guard<mutex> lock(m);
	frame_timer = (double)av_gettime() / 1000000;

	seek_req = 1;
	seek_pos = pos;
	seek_drop = 1;
}

void CP::Forward()
{
	double pos = video_clock;
	pos += 10;
	pos *= AV_TIME_BASE;
	flags = 0;
	SeekPos(pos);
}

void CP::Backward()
{
	double pos = video_clock;
	pos -= 10;
	pos *= AV_TIME_BASE;
	flags = AVSEEK_FLAG_BACKWARD;
	SeekPos(pos);
}

void CP::NextFrame()
{
	step = 1;
	pause = 0;
}

//not working yet
void CP::LastFrame()
{
	step = -1;
}

void CP::HalfPlay()
{
	speed = 0.5;
}

void CP::AudioCallback(void *userdata, Uint8 *stream, int len)
{
	CP *is = (CP *)userdata;
	int len1, audio_size;
	double pts;

	while (len > 0) {
		if (is->audio_buf_index >= is->audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = is->audio_decode_frame(is->audio_buf, sizeof(is->audio_buf), &pts);
			if (audio_size < 0) {
				/* If error, output silence */
				is->audio_buf_size = 1024;
				memset(is->audio_buf, 0, is->audio_buf_size);
			}
			else {
				audio_size = is->synchronize_audio((int16_t *)is->audio_buf,
					audio_size, pts);
				is->audio_buf_size = audio_size;
			}
			is->audio_buf_index = 0;
		}
		len1 = is->audio_buf_size - is->audio_buf_index;
		if (len1 > len)
			len1 = len;
		memcpy(stream, (uint8_t *)is->outa_buffer + is->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		is->audio_buf_index += len1;
	}
}

PlayCore::PlayCore()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		return ;
	}
	avformat_network_init();
}

PlayCore::~PlayCore()
{

}

int PlayCore::runplay(string url)
{
	int ret = -1;

	//avformat_network_init();

	//flush_pkt->data = (uint8_t *)"FLUSH";
	av_init_packet(&cp.flush_pkt);

	/*if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		return -1;
	}*/


	//char filename[] = "D:/work/Resource/oceans.mp4";
	const char *filename = (char *)(url.c_str());
	//memcpy(filename, url.c_str(), sizeof(url.c_str()));

	ret = avformat_open_input(&cp.fmt_ctx, filename, nullptr, nullptr);

	ret = avformat_find_stream_info(cp.fmt_ctx, nullptr);

	av_dump_format(cp.fmt_ctx, 0, filename, 0);

	for (int i = 0; i < cp.fmt_ctx->nb_streams; i++)
	{
		if (cp.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			cp.video_index = i;
		}
		else if (cp.fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			cp.audio_index = i;
		}
	}

	AVCodec *vcodec = nullptr;

	cp.vctx = avcodec_alloc_context3(nullptr);
	if (!cp.vctx)
	{
		return AVERROR(ENOMEM);
	}
	ret = avcodec_parameters_to_context(cp.vctx, cp.fmt_ctx->streams[cp.video_index]->codecpar);
	cp.vctx->pkt_timebase = cp.fmt_ctx->streams[cp.video_index]->time_base;
	vcodec = avcodec_find_decoder(cp.vctx->codec_id);
	avcodec_open2(cp.vctx, vcodec, nullptr);

	AVCodec *acodec = nullptr;
	if (cp.audio_index >= 0)
	{
		cp.actx = avcodec_alloc_context3(nullptr);
		if (!cp.actx)
		{
			return AVERROR(ENOMEM);
		}
		ret = avcodec_parameters_to_context(cp.actx, cp.fmt_ctx->streams[cp.audio_index]->codecpar);
		cp.actx->pkt_timebase = cp.fmt_ctx->streams[cp.audio_index]->time_base;
		acodec = avcodec_find_decoder(cp.actx->codec_id);
		avcodec_open2(cp.actx, acodec, nullptr);
		
		cp.audio_st = cp.fmt_ctx->streams[cp.audio_index];

		cp.SDLConfigureAudio(cp.actx);
#if 0
		QAudioFormat audio_fmt;
		audio_fmt.setSampleRate(cp.actx->sample_rate);	//change by specific condition
		audio_fmt.setChannelCount(cp.actx->channels);
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

		
		cp.out = audio->start();
		cp.med_buffer = (uint8_t*)malloc(MAX_AUDIO_FRAME_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
#endif

#if 0
		cp.swr_ctx = swr_alloc();
		cp.swr_ctx = swr_alloc_set_opts(cp.swr_ctx, cp.actx->channel_layout,
			AV_SAMPLE_FMT_S16,
			cp.actx->sample_rate,
			cp.actx->channels,
			cp.actx->sample_fmt,
			cp.actx->sample_rate,
			0, 0);
		swr_init(cp.swr_ctx);
#endif
	}

	cp.SDLConfiguration(cp.vctx);

	cp.frame_last_delay = 40e-3;
	cp.video_current_pts_time = av_gettime();
	cp.frame_timer = (double)av_gettime() / 1000000.0;


	thread producer(&CP::ParseThread, &cp, ref(cp.fmt_ctx));
	thread consumer(&CP::VideoThread, &cp, ref(cp.vctx));
	thread converter(&CP::DisplayThread, &cp, ref(cp.vctx));
	/*if (cp.audio_index >= 0)
	{
		thread audio(&CP::AudioThread, &cp, ref(cp.actx));
		cp.SDLEventHandle();
		audio.join();
	}*/
	cp.SDLEventHandle();

	producer.join();
	consumer.join();
	converter.join();

	//cp.quit_play();
}

void PlayCore::play(string url)
{
	play_thread = thread(&PlayCore::runplay, this, url);
	//play_thread.join();
}

void PlayCore::control(OPTION option)
{
	switch (option)
	{
	case PlayCore::PAUSE:
		cp.Pause(); break;
	case PlayCore::STOP:
		cp.Stop();	break;
	case PlayCore::FORWARD:
		cp.Forward();	break;
	case PlayCore::BACKWARD:
		cp.Backward();	break;
	case PlayCore::NEXTFRAME:
		cp.NextFrame();	break;
	case PlayCore::LASTFRAME:
		cp.LastFrame();	break;
	default:
		break;
	}
}

