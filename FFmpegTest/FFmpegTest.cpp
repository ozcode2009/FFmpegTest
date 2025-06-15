// Replace the usage of avpicture_get_size and PIX_FMT_RGB24 with updated FFmpeg APIs and fix the typo in pCodexCtx.

#include "FFmpegTest.h"

#define SDL_MAIN_HANDLED
#define SDL_AUDIO_BUFFER_SIZE 1024

#define MAX_AUDIOQ_SIZE (5 * 32 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 512 * 1024)

#define VIDEO_PICTURE_QUEUE_SIZE 4

#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

#define AV_SYNC_THRESHOLD 0.017
#define AV_NOSYNC_THRESHOLD 10.0

#define SAMPLE_CORRECTION_PERCENT_MAX 1
#define AUDIO_DIFF_AVG_NB 20


typedef enum {
	AV_SYNC_AUDIO_MASTER,
	AV_SYNC_VIDEO_MASTER,
	AV_SYNC_EXTERNAL_MASTER,
} AVSyncType;

#define DEFAULT_AV_SYNC_TYPE AV_SYNC_VIDEO_MASTER

#include <chrono>

#include <SDL.h>
#include <SDL_thread.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/time.h>

}

using namespace std;

typedef struct VideoPicture {
	SDL_Texture* bmp;
	int width, height; /* source height & width */
	int allocated;
	double pts;
} VideoPicture;

typedef struct PacketQueue {
	AVFifo* pkt_list;
	int nb_packets;
	int size;
	SDL_mutex* mutex;
	SDL_cond* cond;
} PacketQueue;

typedef struct VideoState {

	AVFormatContext* format_ctx;
	int             videoStream, audioStream;
	AVStream* audio_st;
	AVCodecContext* audio_ctx;
	PacketQueue     audioq;
	uint8_t         audio_buf[(192000 * 3) / 2];
	unsigned int    audio_buf_size;
	unsigned int    audio_buf_index;
	AVPacket        audio_pkt;
	uint8_t* audio_pkt_data;
	int             audio_pkt_size;
	AVStream* video_st;
	AVCodecContext* video_ctx;
	PacketQueue     videoq;

	SwsContext* sws_ctx;
	SwrContext* swr_ctx;

	//We could technically leave the video packet entirely in the video thread but putting it hear to keep things uniform
	AVPacket* audioPacket;
	AVPacket* videoPacket;

	AVPacket* generalPacket;

	double video_clock;
	double frame_last_pts;
	double frame_last_delay;
	double frame_timer;
	double audio_clock;

	double video_current_pts;
	double video_current_pts_time;
	double audio_diff_cum;
	double audio_diff_avg_coef;
	double audio_diff_threshold;
	double audio_diff_avg_count;


	AVSyncType av_sync_type;
	
	AVFrame* pFrame;
	AVFrame* pFrameRescaled;

	VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];
	int             pictq_size, pictq_rindex, pictq_windex;
	SDL_mutex* pictq_mutex;
	SDL_cond* pictq_cond;

	SDL_Thread* parse_tid;
	SDL_Thread* video_tid;

	SDL_AudioSpec spec, wanted_spec;

	SDL_Window* window;
	SDL_Renderer* renderer;

	char            filename[1024];
	int             quit;

	Uint64 next_refresh_time;
	Uint64 perf_frequency;
	int refresh_scheduled;
	std::chrono::milliseconds next_refresh_time_hr;
} VideoState;



void packet_queue_init(PacketQueue* q) {
	
	memset(q, 0, sizeof(PacketQueue));
	// Store AVPacket structs, not pointers
	q->pkt_list = av_fifo_alloc2(1, sizeof(AVPacket*), AV_FIFO_FLAG_AUTO_GROW);
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}
int packet_queue_put(PacketQueue* q, AVPacket* pkt) {
	if (!pkt) return -1;
	AVPacket* pkt1 = av_packet_alloc();
	if (!pkt1) return -1;

	if (av_packet_ref(pkt1, pkt) < 0) {  // Make a reference instead of moving
		av_packet_free(&pkt1);
		return -1;
	}

	SDL_LockMutex(q->mutex);
	int ret = av_fifo_write(q->pkt_list, &pkt1, 1);
	if (ret >= 0) {
		q->nb_packets++;
		q->size += pkt1->size;
		SDL_CondSignal(q->cond);
	}
	else {
		av_packet_unref(pkt1);
		av_packet_free(&pkt1);
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block) {
	int ret;
	AVPacket* pkt1 = nullptr;
	SDL_LockMutex(q->mutex);
	for (;;) {
		if (av_fifo_read(q->pkt_list, &pkt1, 1) >= 0) {
			q->nb_packets--;
			q->size -= pkt1->size;
			av_packet_move_ref(pkt, pkt1);  // This moves the reference
			av_packet_free(&pkt1);          // Free the packet container
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}
double get_audio_clock(VideoState* vs) {
	double pts;
	int hw_buf_size, bytes_per_sec, n;

	pts = vs->audio_clock; /* maintained in the audio thread */
	hw_buf_size = vs->audio_buf_size - vs->audio_buf_index;
	bytes_per_sec = 0;
	n = vs->audio_ctx->ch_layout.nb_channels * 2;
	if (vs->audio_st) {
		bytes_per_sec = vs->audio_ctx->sample_rate * n;
	}
	if (bytes_per_sec) {
		pts -= (double)hw_buf_size / bytes_per_sec;
	}
	return pts;
}
double get_video_clock(VideoState* is) {
	double delta;

	delta = (av_gettime() - is->video_current_pts_time) / 1000000.0;
	return is->video_current_pts + delta;
}

double get_master_clock(VideoState* is) {
	if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
		std::cout << "videoclock: " << get_video_clock(is) << std::endl;
		return get_video_clock(is);
	}
	else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
		return get_audio_clock(is);
	}
	else {
		return 0;// get_external_clock(is);
	}
}
/*static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block) {
	int ret;

	SDL_LockMutex(q->mutex);
	for (;;) {
		// Read packet struct directly from FIFO
		if (av_fifo_read(q->pkt_list, pkt, 1) >= 0) {
			q->nb_packets--;
			q->size -= pkt->size;  // Subtract actual packet data size
			ret = 1;
			break;
		}
		else if (!block) {
			ret = 0;
			break;
		}
		else {
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}*/
double synchronize_video(VideoState* is, AVFrame* src_frame, double pts) {

	double frame_delay;

	if (pts != 0) {
		/* if we have pts, set video clock to it */
		is->video_clock = pts;
	}
	else {
		/* if we aren't given a pts, set it to the clock */
		pts = is->video_clock;
	}
	/* update the video clock */
	frame_delay = av_q2d(is->video_st->time_base);
	//std::cout << "delay: " << frame_delay << std::endl;
	/* if we are repeating a frame, adjust clock according	ly */
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	//frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);
	is->video_clock += frame_delay;
	return pts;
}
int synchronize_audio(VideoState* is, short* samples,
	int samples_size, double pts) {
	int n;
	double ref_clock;

	n = 2 * is->audio_ctx->ch_layout.nb_channels;

	if (is->av_sync_type != AV_SYNC_AUDIO_MASTER) {
		double diff, avg_diff;
		int wanted_size, min_size, max_size, nb_samples;

		ref_clock = get_master_clock(is);
		diff = get_audio_clock(is) - ref_clock;

		if (diff < AV_NOSYNC_THRESHOLD) {
			// accumulate the diffs
			is->audio_diff_cum = diff + is->audio_diff_avg_coef
				* is->audio_diff_cum;
			if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
				is->audio_diff_avg_count++;
			}
			else {
				avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

				if (fabs(avg_diff) >= is->audio_diff_threshold) {
					wanted_size = samples_size +
						((int)(diff * is->audio_ctx->sample_rate) * n);
					min_size = samples_size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX)
						/ 100.0);
					max_size = samples_size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX)
						/ 100.0);
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
						uint8_t* samples_end, * q;
						int nb;

						/* add samples by copying final samples */
						nb = (wanted_size - samples_size);
						samples_end = (uint8_t*)samples + samples_size - n;
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
			is->audio_diff_avg_count = 0;
			is->audio_diff_cum = 0;
		}
	}
	return samples_size;
}

int audio_decode_frame(VideoState* vs, uint8_t* audio_buf, int buf_size, double *pts_ptr) {
	static AVPacket pkt = { 0 };  // Use struct, not pointer
	static AVFrame* frame = NULL;
	static int decoder_has_data = 0;
	static int pkt_valid = 0;  // Track if we have a valid packet
	double pts;
	int n;

	// Initialize frame on first call
	if (!frame) {
		frame = av_frame_alloc();
		if (!frame) return -1;
	}

	for (;;) {
		// First, try to get a frame from the decoder
		int ret = avcodec_receive_frame(vs->audio_ctx, frame);

		if (ret == 0) {
			int data_size = av_samples_get_buffer_size(
				NULL,
				frame->ch_layout.nb_channels,
				frame->nb_samples,
				(AVSampleFormat)frame->format,
				1
			);

			pts = vs->audio_clock;
			*pts_ptr = pts;
			n = 2 * vs->audio_ctx->ch_layout.nb_channels;
			vs->audio_clock += (double)data_size /
				(double)(n * vs->audio_ctx->sample_rate);

			if (data_size <= 0 || data_size > buf_size) {
				av_frame_unref(frame);
				continue;  // Skip invalid frames
			}

			// Handle planar vs packed audio formats
			if (av_sample_fmt_is_planar((AVSampleFormat)frame->format)) {
				// For planar audio, need to interleave channels
				int bytes_per_sample = av_get_bytes_per_sample((AVSampleFormat)frame->format);
				int channels = frame->ch_layout.nb_channels;
				uint8_t* dst = audio_buf;

				for (int i = 0; i < frame->nb_samples; i++) {
					for (int ch = 0; ch < channels; ch++) {
						memcpy(dst, frame->data[ch] + i * bytes_per_sample, bytes_per_sample);
						dst += bytes_per_sample;
					}
				}
			}
			else {
				// Packed audio - direct copy
				memcpy(audio_buf, frame->data[0], data_size);
			}

			av_frame_unref(frame);
			return data_size;

		}
		else if (ret == AVERROR(EAGAIN)) {
			// Decoder needs more input
			decoder_has_data = 0;

		}
		else if (ret == AVERROR_EOF) {
			// End of stream
			return -1;

		}
		else {
			// Other error - try to continue
			decoder_has_data = 0;
		}

		// Need to send more data to decoder
		if (!decoder_has_data) {
			// Clean up previous packet if we have one
			if (pkt_valid) {
				av_packet_unref(&pkt);
				pkt_valid = 0;
			}

			if (packet_queue_get(&vs->audioq, &pkt, 1) <= 0) {
				return -1;
			}
			pkt_valid = 1;

			if (pkt.pts != AV_NOPTS_VALUE) {
				std::cout << "framepts: " << pkt.pts << std::endl;
				vs->audio_clock = av_q2d(vs->audio_st->time_base) * pkt.pts;
			}

			ret = avcodec_send_packet(vs->audio_ctx, &pkt);

			if (ret < 0) {
				if (ret == AVERROR(EAGAIN)) {
					// Decoder is full, try to receive frames first
					continue;
				}
				else if (ret == AVERROR_EOF) {
					return -1;
				}
				else {
					// Skip this packet on other errors
					av_packet_unref(&pkt);
					pkt_valid = 0;
					continue;
				}
			}

			decoder_has_data = 1;
		}
	}

}

void audio_callback(void* userdata, Uint8* stream, int len) {
	VideoState* vs = (VideoState*)userdata;
	int len1, audio_size;
	double pts;

	while (len > 0) {
		if (vs->audio_buf_index >= vs->audio_buf_size) {
			// Decode a new audio frame into vs->audio_buf.
			audio_size = audio_decode_frame(vs, vs->audio_buf, sizeof(vs->audio_buf), &pts);
			if (audio_size < 0) {
				// If error, output silence.
				vs->audio_buf_size = 1024;
				memset(vs->audio_buf, 0, vs->audio_buf_size);
				std::cout << "nothing" << std::endl;
			}
			else {
				// Synchronize audio using buffer in VideoState.
				audio_size = synchronize_audio(vs, (int16_t*)vs->audio_buf, audio_size, pts);
				vs->audio_buf_size = audio_size;
			}
			vs->audio_buf_index = 0;
		}

		len1 = vs->audio_buf_size - vs->audio_buf_index;
		if (len1 > len)
			len1 = len;

		memcpy(stream, vs->audio_buf + vs->audio_buf_index, len1);
		len -= len1;
		stream += len1;
		vs->audio_buf_index += len1;
	}
}
void alloc_picture(void* userdata) {

	VideoState* vs = (VideoState*)userdata;
	VideoPicture* vp;

	vp = &vs->pictq[vs->pictq_windex];
	if (vp->bmp) {
		// we already have one make another, bigger/smaller
		SDL_DestroyTexture(vp->bmp);
	}
	// Allocate a place to put our YUV image on that screen
	vp->bmp = SDL_CreateTexture(
		vs->renderer,
		SDL_PIXELFORMAT_YV12,
		SDL_TEXTUREACCESS_STREAMING,
		1280,
		720
	);
	vp->width = vs->video_ctx->width;
	vp->height = vs->video_ctx->height;
	vp->allocated = 1;
}
int queue_picture(VideoState* vs, AVFrame* pFrame, double pts) {

	VideoPicture* vp;

	/* wait until we have space for a new pic */
	SDL_LockMutex(vs->pictq_mutex);
	while (vs->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&
		!vs->quit) {
		SDL_CondWait(vs->pictq_cond, vs->pictq_mutex);
	}
	SDL_UnlockMutex(vs->pictq_mutex);

	if (vs->quit)
		return -1;

	// windex is set to 0 initially
	vp = &vs->pictq[vs->pictq_windex];

	/* allocate or resize the buffer! */
	if (!vp->bmp ||
		vp->width != vs->video_ctx->width ||
		vp->height != vs->video_ctx->height) {
		SDL_Event event;

		vp->allocated = 0;
		alloc_picture(vs);
		if (vs->quit) {
			return -1;
		}
	}
	if (vp->bmp) {
		uint8_t* pixels = nullptr;
		int pitch = 0;
		SDL_LockTexture(vp->bmp, NULL, (void**)&pixels, &pitch);
		uint8_t* dst_data[4] = { 0 };
		int dst_linesize[4] = { 0 };

		// Calculate plane offsets for YUV420P
		int width = 1280;   // Use source width
		int height = 720; // Use source height

		dst_data[0] = pixels;                                    // Y plane
		dst_data[2] = pixels + (width * height);                 // U plane (swapped with V)
		dst_data[1] = pixels + (width * height * 5 / 4);        // V plane (swapped with U)

		dst_linesize[0] = pitch;                                // Y pitch
		dst_linesize[1] = dst_linesize[2] = pitch / 2;

		sws_scale(vs->sws_ctx, (uint8_t const* const*)pFrame->data,
			pFrame->linesize, 0, vs->video_ctx->height, dst_data,
			dst_linesize);

		vp->pts = pts;

		SDL_UnlockTexture(vp->bmp);
		if (++vs->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE) {
			vs->pictq_windex = 0;
		}
		SDL_LockMutex(vs->pictq_mutex);
		vs->pictq_size++;
		SDL_UnlockMutex(vs->pictq_mutex);
	}
}


int video_thread(void* arg) {
	VideoState* vs = (VideoState*)arg;
	int frameFinished;
	double pts;
	int innerFail = 0;

	vs->pFrame = av_frame_alloc();
	vs->videoPacket = av_packet_alloc();

	for (;;) {
		if (packet_queue_get(&vs->videoq, vs->videoPacket, 1) < 0) {
			// means we quit getting packets
			std::cout << "break" << std::endl;
			break;
		}
		pts = 0;
		// Decode video frame
		int ret = avcodec_send_packet(vs->video_ctx, vs->videoPacket);
		if (ret < 0) {
			char err_buf[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(ret, err_buf, AV_ERROR_MAX_STRING_SIZE);
			cerr << "Could not send packet: " << err_buf << endl;
			return -1;
		}
		for (;;) {
			ret = avcodec_receive_frame(vs->video_ctx, vs->pFrame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				break;  // No more frames for this packet
			} else if (ret < 0) {
				continue;
			}
			
			if (vs->pFrame->pts != AV_NOPTS_VALUE) {
				pts = vs->pFrame->pts * av_q2d(vs->video_st->time_base);
			}
			else {
				pts = 0; // Or use a fallback like frame count * frame duration
			}

			pts = synchronize_video(vs, vs->pFrame, pts);

			// Did we get a video frame?
			if (queue_picture(vs, vs->pFrame, pts) < 0) {
				innerFail = 1;
				break;
			}
		}
		av_packet_unref(vs->videoPacket);
		if (innerFail) {
			//break;
		}

	}
	av_frame_free(&vs->pFrame);
	av_packet_free(&vs->videoPacket);
	return 0;
}
int stream_component_open(VideoState* vs, int stream_index) {
	SDL_AudioSpec wanted_spec, spec;

	if (stream_index < 0 || stream_index >= vs->format_ctx->nb_streams) {
		return -1;
	}

	//These two need not be freed
	AVCodecParameters* codecpar = vs->format_ctx->streams[stream_index]->codecpar;
	const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);

	if (codec == nullptr) {
		cerr << "Unsupported codec!" << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	AVCodecContext* codecCtx = avcodec_alloc_context3(codec);

	if (codecCtx == nullptr) {
		cerr << "Could not allocate codec context." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	avcodec_parameters_to_context(codecCtx, codecpar);
	if (avcodec_open2(codecCtx, codec, NULL) < 0) {
		cerr << "Could not open codec." << endl;
		avcodec_free_context(&codecCtx);
		return -1;
	}

	switch (codecCtx->codec_type) {
	case AVMEDIA_TYPE_AUDIO:
		vs->audioStream = stream_index;
		vs->audio_st = vs->format_ctx->streams[stream_index];
		vs->audio_ctx = codecCtx;
		vs->audio_buf_size = 0;
		vs->audio_buf_index = 0;

		vs->wanted_spec.freq = vs->audio_ctx->sample_rate;
		vs->wanted_spec.format = AUDIO_S16SYS;
		vs->wanted_spec.channels = vs->audio_ctx->ch_layout.nb_channels;
		vs->wanted_spec.silence = 0;
		vs->wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		vs->wanted_spec.callback = audio_callback;
		vs->wanted_spec.userdata = vs;

		vs->audio_diff_avg_coef = exp(log(0.01 / AUDIO_DIFF_AVG_NB));
		vs->audio_diff_threshold = 2.0 * SDL_AUDIO_BUFFER_SIZE / codecCtx->sample_rate;
		vs->audio_diff_avg_count = 0;

		packet_queue_init(&vs->audioq);

		if (SDL_OpenAudio(&vs->wanted_spec, &vs->spec) < 0) {
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}
		memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
		packet_queue_init(&vs->audioq);

		SDL_PauseAudio(0);

		break;
	case AVMEDIA_TYPE_VIDEO:
		vs->frame_timer = (double)av_gettime() / 1000000.0;
		vs->frame_last_delay = 16e-3;
		vs->videoStream = stream_index;
		vs->video_clock = 0;
		vs->video_st = vs->format_ctx->streams[stream_index];
		vs->video_ctx = codecCtx;
		vs->video_current_pts_time = av_gettime();

		packet_queue_init(&vs->videoq);
		vs->video_tid = SDL_CreateThread(video_thread, "video_thread", vs);
		vs->sws_ctx = sws_getContext(vs->video_ctx->width,
			vs->video_ctx->height,
			vs->video_ctx->pix_fmt,
			1280,
			720,
			AV_PIX_FMT_YUV420P,
			SWS_BILINEAR,
			nullptr, nullptr, nullptr);
		break;
	default:
		break;
	}
}
int decode_thread(void* data) {
	VideoState* vs = (VideoState*)data;

	vs->generalPacket = av_packet_alloc();

	if (avformat_open_input(&vs->format_ctx, /*vs->filename*/ "video.mp4", nullptr, nullptr) != 0) {
		cerr << "Could not open input file." << endl;
		return -1;
	}
	if (avformat_find_stream_info(vs->format_ctx, nullptr) < 0) {
		cerr << "Could not find stream information." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}
	vs->audioStream = -1;
	vs->videoStream = -1;
	for (unsigned int i = 0; i < vs->format_ctx->nb_streams; i++) {
		if (vs->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vs->videoStream < 0) {
			vs->videoStream = i;

		}
		if (vs->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && vs->audioStream < 0) {
			vs->audioStream = i;
		}
	}

	vs->audioPacket = av_packet_alloc();

	if (vs->videoStream == -1) {
		cerr << "Could not find a video stream." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}
	if (vs->audioStream == -1) {
		cerr << "Could not find an audio stream." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	stream_component_open(vs, vs->videoStream);
	stream_component_open(vs, vs->audioStream);

	for (;;) {
		if (vs->quit) {
			break;
		}
		// seek stuff goes here
		if (vs->audioq.size > MAX_AUDIOQ_SIZE ||
			vs->videoq.size > MAX_VIDEOQ_SIZE) {
			SDL_Delay(10);
			continue;
		}
		if (av_read_frame(vs->format_ctx, vs->generalPacket) < 0) {
			if ((vs->format_ctx->pb->error) == 0) {
				SDL_Delay(100); /* no error; wait for user input */
				continue;
			}
			else {
				break;
			}
		}
		// Is this a packet from the video stream?
		if (vs->generalPacket->stream_index == vs->videoStream) {
			packet_queue_put(&vs->videoq, vs->generalPacket);
		} else if (vs->generalPacket->stream_index == vs->audioStream) {
			packet_queue_put(&vs->audioq, vs->generalPacket);
		}
		else {
			av_packet_unref(vs->audioPacket);
		}
	}
	while (!vs->quit) {
		SDL_Delay(100);
	}

	if (1) {
		SDL_Event event;
		event.type = FF_QUIT_EVENT;
		event.user.data1 = vs;
		SDL_PushEvent(&event);
	}
	av_packet_free(&vs->generalPacket);
	return 0;

}


static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque) {
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0; /* 0 means stop timer */
}

static void schedule_refresh_precise(VideoState* is, int delay_ms) {
	// Use more precise timing
    // Fix the error by converting the time_point to milliseconds using std::chrono::duration_cast.
    std::chrono::milliseconds now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch());
	auto delay = std::chrono::milliseconds(delay_ms);
	is->next_refresh_time_hr = now + delay;
	is->refresh_scheduled = 1;
}

int should_refresh_video_precise(VideoState* is) {
	if (!is->refresh_scheduled) return 0;

	auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now().time_since_epoch());
	if (now >= is->next_refresh_time_hr) {
		is->refresh_scheduled = 0;
		return 1;
	}
	return 0;
}




void video_display(VideoState* vs) {
	SDL_Rect rect;
	VideoPicture* vp;
	float aspect_ratio;
	int w, h, x, y;

	vp = &vs->pictq[vs->pictq_rindex];
	if (vp->bmp) {  // SDL_Texture* but keeping original name
		// Calculate aspect ratio (same logic)
		if (vs->video_st->codecpar->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		}
		else {
			aspect_ratio = av_q2d(vs->video_st->codecpar->sample_aspect_ratio) *
				vs->video_st->codecpar->width / vs->video_st->codecpar->height;
		}
		if (aspect_ratio <= 0.0) {
			aspect_ratio = (float)vs->video_st->codecpar->width /
				(float)vs->video_st->codecpar->height;
		}

		// Get current window size
		int screen_w, screen_h;
		SDL_GetWindowSize(vs->window, &screen_w, &screen_h);

		// Calculate display rectangle (same scaling logic)
		h = screen_h;
		w = ((int)rint(h * aspect_ratio)) & -3;
		if (w > screen_w) {
			w = screen_w;
			h = ((int)rint(w / aspect_ratio)) & -3;
		}
		x = (screen_w - w) / 2;
		y = (screen_h - h) / 2;

		rect.x = x;
		rect.y = y;
		rect.w = w;
		rect.h = h;

		// Modern SDL2 rendering
		SDL_SetRenderDrawColor(vs->renderer, 0, 0, 0, 255);  // Black background
		SDL_RenderClear(vs->renderer);                        // Creates the black bars
		SDL_RenderCopy(vs->renderer, vp->bmp, NULL, &rect);  // Draw video in calculated rect
		SDL_RenderPresent(vs->renderer);
	}
}
void video_refresh_timer(void* userdata) {
	VideoState* is = (VideoState*)userdata;
	VideoPicture* vp;
	double actual_delay, delay, sync_threshold, ref_clock, diff;

	// If no video stream, just schedule next refresh
	if (!is->video_st) {
		schedule_refresh_precise(is, 100);
		return;
	}

	// If no frames ready, wait a bit
	if (is->pictq_size == 0) {
		schedule_refresh_precise(is, 1);
		return;
	}

	// Get current frame
	vp = &is->pictq[is->pictq_rindex];

	// Update video clock
	is->video_current_pts = vp->pts;
	is->video_current_pts_time = av_gettime();

	// Calculate delay from PTS difference
	delay = vp->pts - is->frame_last_pts;

	// If delay is invalid, use previous delay or fallback to frame rate
	if (delay <= 0 || delay >= 1.0) {
		if (is->frame_last_delay > 0) {
			delay = is->frame_last_delay;
		}
		else {
			// Fallback to video frame rate
			double target_fps = 60.0;
			if (is->video_st && is->video_st->r_frame_rate.num > 0) {
				target_fps = av_q2d(is->video_st->r_frame_rate);
			}
			else if (is->video_st && is->video_st->avg_frame_rate.num > 0) {
				target_fps = av_q2d(is->video_st->avg_frame_rate);
			}
			delay = 1.0 / target_fps;
		}
	}

	// Save for next time
	is->frame_last_delay = delay;
	is->frame_last_pts = vp->pts;

	// Sync to master clock (usually audio)
	if (is->av_sync_type != AV_SYNC_VIDEO_MASTER) {
		ref_clock = get_master_clock(is);
		diff = vp->pts - ref_clock;

		// Sync threshold - use delay or minimum threshold
		sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;

		// Only sync if difference is not too large (avoid wild corrections)
		//std::cout << "diff: " << diff << std::endl;
		if (fabs(diff) < AV_NOSYNC_THRESHOLD) {
			if (diff <= -sync_threshold) {
				std::cout << "behind" << std::endl;
				// Video is behind audio - show frame immediately
				delay = 0;
			}
			else if (diff >= sync_threshold) {
				std::cout << "ahead" << std::endl;
				// Video is ahead of audio - delay more
				delay = delay + diff;
			}
			// If diff is within threshold, use original delay
		}
	}

	// High precision timing integration
	Uint64 current_time = SDL_GetPerformanceCounter();
	double current_time_sec = (double)current_time / is->perf_frequency;

	// Initialize frame timer if needed
	if (is->frame_timer == 0) {
		is->frame_timer = current_time_sec;
	}

	// Update frame timer for this frame
	is->frame_timer += delay;

	// Calculate actual delay until frame should be shown
	actual_delay = is->frame_timer - current_time_sec;

	// Clamp delay to reasonable bounds
	if (actual_delay < 0.010) {
		// Behind schedule - show immediately but with minimum delay
		actual_delay = 0.010;
		// Reset timer to prevent accumulated drift
		is->frame_timer = current_time_sec + actual_delay;
	}
	else if (actual_delay > 0.1) {
		// Too far ahead - cap delay
		actual_delay = 0.1;
		is->frame_timer = current_time_sec + actual_delay;
	}

	// Schedule next refresh with calculated delay
	schedule_refresh_precise(is, (int)(actual_delay * 1000 + 0.5));

	// Display the frame
	video_display(is);

	// Advance queue
	SDL_LockMutex(is->pictq_mutex);
	if (++is->pictq_rindex >= VIDEO_PICTURE_QUEUE_SIZE) {
		is->pictq_rindex = 0;
	}
	is->pictq_size--;
	SDL_CondSignal(is->pictq_cond);
	SDL_UnlockMutex(is->pictq_mutex);
}

int main() {
	SDL_Event event;
	VideoState* vs;


	vs = (VideoState*)av_mallocz(sizeof(VideoState));
	av_strlcpy(vs->filename, "video2.mp4", sizeof(vs->filename));

	vs->pictq_mutex = SDL_CreateMutex();
	vs->pictq_cond = SDL_CreateCond();

	vs->av_sync_type = DEFAULT_AV_SYNC_TYPE;

	vs->perf_frequency = SDL_GetPerformanceFrequency();
	vs->next_refresh_time = 0;
	vs->refresh_scheduled = 0;


	schedule_refresh_precise(vs, 16);

	vs->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", vs);
	if (!vs->parse_tid) {
		av_free(vs);
		return -1;
	}
	//Init SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}
	vs->window = SDL_CreateWindow("Basic C SDL project",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		1280, 720,
		SDL_WINDOW_SHOWN);

	vs->renderer = SDL_CreateRenderer(vs->window, -1, SDL_RENDERER_ACCELERATED);
	if (!vs->renderer) {
		fprintf(stderr, "SDL: could not create renderer - exiting\n");
		exit(1);
	}



	for (;;) {
		/*SDL_WaitEvent(&event);
		switch (event.type) {
		case FF_REFRESH_EVENT:
			video_refresh_timer(event.user.data1);
			break;
		default:
			break;
		}*/

		if (should_refresh_video_precise(vs)) {
			video_refresh_timer(vs);
		}

	}
	if (vs->videoPacket) {
		av_packet_free(&vs->videoPacket);
	}
	if (vs->audioPacket) {
		av_packet_free(&vs->audioPacket);
	}
	if (vs->sws_ctx) {
		sws_freeContext(vs->sws_ctx);
	}
	if (vs->video_ctx) {
		avcodec_free_context(&vs->video_ctx);
	}
	if (vs->format_ctx) {
		avformat_close_input(&vs->format_ctx);
	}
	return 0;
}
