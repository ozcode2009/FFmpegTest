// Replace the usage of avpicture_get_size and PIX_FMT_RGB24 with updated FFmpeg APIs and fix the typo in pCodexCtx.

#include "FFmpegTest.h"

#define SDL_MAIN_HANDLED
#define SDL_AUDIO_BUFFER_SIZE 1024

#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)\

#define VIDEO_PICTURE_QUEUE_SIZE 2

#define FF_ALLOC_EVENT   (SDL_USEREVENT)
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)



extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/fifo.h>
#include <SDL.h>
#include <SDL_thread.h>
#include <libavutil/avstring.h>
}

using namespace std;

typedef struct VideoPicture {
	SDL_Texture* bmp;
	int width, height; /* source height & width */
	int allocated;
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
} VideoState;



void packet_queue_init(PacketQueue* q) {
	
	memset(q, 0, sizeof(PacketQueue));
	// Store AVPacket structs, not pointers
	q->pkt_list = av_fifo_alloc2(1, sizeof(AVPacket), AV_FIFO_FLAG_AUTO_GROW);
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
	std::cout << "fifo" << q->pkt_list << std::endl;
}

int packet_queue_put(PacketQueue* q, AVPacket* pkt) {
	if (!pkt) return -1;

	SDL_LockMutex(q->mutex);

	// Write the packet struct directly to FIFO
	int ret = av_fifo_write(q->pkt_list, pkt, 1);
	if (ret >= 0) {
		q->nb_packets++;
		q->size += pkt->size;  // Add actual packet data size, not pointer size
		SDL_CondSignal(q->cond);
	}

	SDL_UnlockMutex(q->mutex);
	return ret;
}

static int packet_queue_get(PacketQueue* q, AVPacket* pkt, int block) {
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
}

int audio_decode_frame(VideoState* vs, uint8_t* audio_buf, int buf_size) {
	static AVPacket pkt = { 0 };  // Use struct, not pointer
	static AVFrame* frame = NULL;
	static int decoder_has_data = 0;
	static int pkt_valid = 0;  // Track if we have a valid packet

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
	static uint8_t audio_buf[(192000 * 3) / 2];
	static unsigned int audio_buf_size = 0;
	static unsigned int audio_buf_index = 0;

	while (len > 0) {
		if (audio_buf_index >= audio_buf_size) {
			/* We have already sent all our data; get more */
			audio_size = audio_decode_frame(vs, audio_buf, sizeof(audio_buf));
			if (audio_size < 0) {
				/* If error, output silence */
				audio_buf_size = 1024;
				memset(audio_buf, 0, audio_buf_size);
			}
			else {
				audio_buf_size = audio_size;
			}
			audio_buf_index = 0;
		}

		len1 = audio_buf_size - audio_buf_index;
		if (len1 > len)
			len1 = len;

		memcpy(stream, (uint8_t*)audio_buf + audio_buf_index, len1);
		len -= len1;
		stream += len1;
		audio_buf_index += len1;
	}
}
int decode_thread(void* data) {
	VideoState* vs = (VideoState*)data;
	if (avformat_open_input(&vs->format_ctx, vs->filename, nullptr, nullptr) != 0) {
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
		if (av_read_frame(vs->format_ctx, vs->audioPacket) < 0) {
			if ((vs->format_ctx->pb->error) == 0) {
				SDL_Delay(100); /* no error; wait for user input */
				continue;
			}
			else {
				break;
			}
		}
		// Is this a packet from the video stream?
		if (vs->audioPacket->stream_index == vs->videoStream) {
			packet_queue_put(&vs->videoq, vs->audioPacket);
		} else if (vs->audioPacket->stream_index == vs->audioStream) {
			packet_queue_put(&vs->audioq, vs->audioPacket);
		}
		else {
			av_packet_unref(vs->audioPacket);
		}
	}
	while (!vs->quit) {
		SDL_Delay(100);
	}

fail:
	if (1) {
		SDL_Event event;
		event.type = FF_QUIT_EVENT;
		event.user.data1 = vs;
		SDL_PushEvent(&event);
	}
	return 0;

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
		renderer,
		SDL_PIXELFORMAT_YV12,
		SDL_TEXTUREACCESS_STREAMING,
		1280,
		720
	);
	vp->width = vs->video_ctx->width;
	vp->height = vs->video_ctx->height;
	vp->allocated = 1;
}
int queue_picture(VideoState* vs, AVFrame* pFrame) {

	VideoPicture* vp;
	int dst_pix_fmt;

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
		SDL_LockTexture(vp->bmp, NULL, (void **) &pixels, &pitch);
		uint8_t* dst_data[4] = { 0 };
		int dst_linesize[4] = { 0 };

		// Calculate plane offsets for YUV420P
		int width = 1280;
		int height = 720;

		dst_data[0] = (uint8_t*)pixels;                           // Y plane
		dst_data[1] = dst_data[0] + (width * height);             // U plane
		dst_data[2] = dst_data[1] + (width * height / 4);         // V plane

		dst_linesize[0] = pitch;                                  // Y pitch
		dst_linesize[1] = dst_linesize[2] = width / 2;           // U/V pitch
		sws_scale(vs->sws_ctx, (uint8_t const* const*)pFrame->data,
			pFrame->linesize, 0, vs->video_ctx->height, dst_data,
			dst_linesize);
		//left here as backup in case the thing doesn't work.
		/*SDL_UpdateYUVTexture(
			texture,
			NULL,
			pFrameRGB->data[0],  // Y plane
			pFrameRGB->linesize[0],  // Y pitch
			pFrameRGB->data[1],  // U plane  
			pFrameRGB->linesize[1],  // U pitch
			pFrameRGB->data[2],  // V plane
			pFrameRGB->linesize[2]   // V pitch
		);*/

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

	vs->pFrame = av_frame_alloc();
	vs->videoPacket = av_packet_alloc();

	for (;;) {
		if (packet_queue_get(&vs->videoq, vs->videoPacket, 1) < 0) {
			// means we quit getting packets
			break;
		}
		// Decode video frame
		int ret = avcodec_send_packet(vs->video_ctx, vs->videoPacket);
		if (ret < 0) {
			cerr << "Could not send packet";
			return -1;
		}
		ret = avcodec_receive_frame(vs->video_ctx, vs->pFrame);
		if (ret < 0) {
			continue;
		}

		// Did we get a video frame?
		if (queue_picture(vs, vs->pFrame) < 0) {
			break;
		}
		av_packet_unref(vs->videoPacket);
	}
	av_frame_free(&vs->pFrame);
	av_packet_free(&vs->videoPacket);
	return 0;
}

static void schedule_refresh(VideoState* is, int delay) {
	SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque) {
	SDL_Event event;
	event.type = FF_REFRESH_EVENT;
	event.user.data1 = opaque;
	SDL_PushEvent(&event);
	return 0; /* 0 means stop timer */
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
		vs->wanted_spec.freq = vs->audio_ctx->sample_rate;
		vs->wanted_spec.format = AUDIO_S16SYS;
		vs->wanted_spec.channels = vs->audio_ctx->ch_layout.nb_channels;
		vs->wanted_spec.silence = 0;
		vs->wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
		vs->wanted_spec.callback = audio_callback;
		vs->wanted_spec.userdata = vs;

		packet_queue_init(&vs->audioq);

		if (SDL_OpenAudio(&vs->wanted_spec, &vs->spec) < 0) {
			fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
			return -1;
		}

		vs->audioStream = stream_index;
		vs->audio_st = vs->format_ctx->streams[stream_index];
		vs->audio_ctx = codecCtx;
		vs->audio_buf_size = 0;
		vs->audio_buf_index = 0;
		memset(&vs->audio_pkt, 0, sizeof(vs->audio_pkt));
		packet_queue_init(&vs->audioq);

		SDL_PauseAudio(0);

		break;
	case AVMEDIA_TYPE_VIDEO:
		vs->videoStream = stream_index;
		vs->video_st = vs->format_ctx->streams[stream_index];
		vs->video_ctx = codecCtx;

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

void video_refresh_timer(void* userdata) {

	VideoState* vs = (VideoState*)userdata;
	VideoPicture* vp;

	if (vs->video_st) {
		if (vs->pictq_size == 0) {
			schedule_refresh(vs, 1);
		}
		else {
			vp = &vs->pictq[vs->pictq_rindex];
			/* Timing code goes here */

			schedule_refresh(vs, 80);

			/* show the picture! */
			video_display(vs);

			/* update queue for next picture! */
			if (++vs->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE) {
				vs->pictq_rindex = 0;
			}
			SDL_LockMutex(vs->pictq_mutex);
			vs->pictq_size--;
			SDL_CondSignal(vs->pictq_cond);
			SDL_UnlockMutex(vs->pictq_mutex);
		}
	}
	else {
		schedule_refresh(vs, 100);
	}
}

void video_display(VideoState* is) {
	SDL_Rect rect;
	VideoPicture* vp;
	float aspect_ratio;
	int w, h, x, y;

	vp = &is->pictq[is->pictq_rindex];
	if (vp->bmp) {  // SDL_Texture* but keeping original name
		// Calculate aspect ratio (same logic)
		if (is->video_st->codecpar->sample_aspect_ratio.num == 0) {
			aspect_ratio = 0;
		}
		else {
			aspect_ratio = av_q2d(is->video_st->codecpar->sample_aspect_ratio) *
				is->video_st->codecpar->width / is->video_st->codecpar->height;
		}
		if (aspect_ratio <= 0.0) {
			aspect_ratio = (float)is->video_st->codecpar->width /
				(float)is->video_st->codecpar->height;
		}

		// Get current window size
		int screen_w, screen_h;
		SDL_GetWindowSize(window, &screen_w, &screen_h);

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
		SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);  // Black background
		SDL_RenderClear(renderer);                        // Creates the black bars
		SDL_RenderCopy(renderer, vp->bmp, NULL, &rect);  // Draw video in calculated rect
		SDL_RenderPresent(renderer);
	}
}


int main()
{

	SDL_Event event;
	VideoState* vs;
	
	vs = (VideoState*) av_mallocz(sizeof(VideoState));
	av_strlcpy(vs->filename, "video.mp4", sizeof(vs->filename));

	vs->pictq_mutex = SDL_CreateMutex();
	vs->pictq_cond = SDL_CreateCond();

	schedule_refresh(vs, 40);

	vs->parse_tid = SDL_CreateThread(decode_thread, "decode_thread", vs);
	if (!vs->parse_tid) {
		av_free(vs);
		return -1;
	}

	//Init ffmpeg

	



	vs->video_codecpar = vs->format_ctx->streams[vs->videoStream]->codecpar;
	vs->audio_codecpar = vs->format_ctx->streams[vs->audioStream]->codecpar;

	vs->video_codec = avcodec_find_decoder(vs->video_codecpar->codec_id);

	if(vs->video_codec == nullptr) {
		cerr << "Unsupported video codec!" << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	vs->audio_codec = avcodec_find_decoder(vs->audio_codecpar->codec_id);
	
	if (vs->audio_codec == nullptr) {
		cerr << "Unsupported audio codec!" << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	vs->video_ctx = avcodec_alloc_context3(vs->video_codec);

	if(vs->video_ctx == nullptr) {
		cerr << "Could not allocate video codec context." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}
	avcodec_parameters_to_context(vs->video_ctx, vs->video_codecpar);
	if(avcodec_open2(vs->video_ctx, vs->video_codec, nullptr) < 0) {
		cerr << "Could not open video codec." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	vs->audio_ctx = avcodec_alloc_context3(vs->audio_codec);

	if(vs->audio_ctx == nullptr) {
		cerr << "Could not allocate audio codec context." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}
	avcodec_parameters_to_context(vs->audio_ctx, vs->audio_codecpar);
	if(avcodec_open2(vs->audio_ctx, vs->audio_codec, nullptr) < 0) {
		cerr << "Could not open audio codec." << endl;
		avformat_close_input(&vs->format_ctx);
		return -1;
	}

	vs->wanted_spec.freq = vs->audio_ctx->sample_rate;
	vs->wanted_spec.format = AUDIO_S16SYS;
	vs->wanted_spec.channels = vs->audio_ctx->ch_layout.nb_channels;
	vs->wanted_spec.silence = 0;
	vs->wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
	vs->wanted_spec.callback = audio_callback;
	vs->wanted_spec.userdata = vs;

	packet_queue_init(&vs->audioq);

	if (SDL_OpenAudio(&vs->wanted_spec, &vs->spec) < 0) {
		fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
		return -1;
	}
	SDL_PauseAudio(0);
	
	AVFrame* pFrame = nullptr;
	pFrame = av_frame_alloc();

	if (pFrame == nullptr) {
		cerr << "Could not allocate frame." << endl;
		return -1;
	}

	AVFrame* pFrameRGB = nullptr;
	pFrameRGB = av_frame_alloc();

	if (pFrameRGB == nullptr) {
		cerr << "Could not allocate frame." << endl;
		return -1;
	}
	int numBytes = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, 1280, 720, 1);
	uint8_t* buffer = (uint8_t*)av_malloc(numBytes * sizeof(uint8_t));
	av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize, buffer, AV_PIX_FMT_YUV420P, 1280, 720, 1);

	struct SwsContext* sws_ctx = nullptr;
	int frameFinished;
	AVPacket* pPacket = av_packet_alloc();


	sws_ctx = sws_getContext(vs->video_ctx->width,
		vs->video_ctx->height,
		vs->video_ctx->pix_fmt,
		1280,
		720,
		AV_PIX_FMT_YUV420P,
		SWS_BILINEAR,
		nullptr, nullptr, nullptr);

	//Init SDL
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		fprintf(stderr, "Could not initialize SDL - %s\n", SDL_GetError());
		exit(1);
	}
	SDL_Window* window = SDL_CreateWindow("Basic C SDL project",
		SDL_WINDOWPOS_UNDEFINED,
		SDL_WINDOWPOS_UNDEFINED,
		1280, 720,
		SDL_WINDOW_SHOWN);

	SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
	if (!renderer) {
		fprintf(stderr, "SDL: could not create renderer - exiting\n");
		exit(1);
	}



	int i = 0;
	while (av_read_frame(vs->format_ctx, pPacket) >= 0) {
		if (pPacket->stream_index == vs->videoStream) {
			int ret = avcodec_send_packet(vs->video_ctx, pPacket);
			if (ret < 0) {
				cerr << "Could not send packet";
				return -1;
			}
			ret = avcodec_receive_frame(vs->video_ctx, pFrame);
			if (ret < 0) {
				continue;
			}
		

			// Convert the image into YUV format that SDL uses
			sws_scale(sws_ctx, (uint8_t const* const*)pFrame->data,
				pFrame->linesize, 0, vs->video_ctx->height, pFrameRGB->data,
				pFrameRGB->linesize);

			SDL_UpdateYUVTexture(
				texture,
				NULL,
				pFrameRGB->data[0],  // Y plane
				pFrameRGB->linesize[0],  // Y pitch
				pFrameRGB->data[1],  // U plane  
				pFrameRGB->linesize[1],  // U pitch
				pFrameRGB->data[2],  // V plane
				pFrameRGB->linesize[2]   // V pitch
			);

			SDL_RenderClear(renderer);
			SDL_RenderCopy(renderer, texture, NULL, NULL);
			SDL_RenderPresent(renderer);

			av_packet_unref(pPacket);
		}
		else if (pPacket->stream_index == vs->audioStream) {
			packet_queue_put(&vs->audioq, pPacket);
			
		}
		else {
			//std::cout << "Audio packet queued." << std::endl;
			av_packet_unref(pPacket);
		}

		i++;
	}

	for (;;) {
		SDL_WaitEvent(&event);
		switch (event.type) {
		case FF_REFRESH_EVENT: {
			video_refresh_timer(event.user.data1);
			break;
		default:
			break;
		}
	}

	if (pPacket) {
		av_packet_free(&pPacket);
	}
	if (sws_ctx) {
		sws_freeContext(sws_ctx);
	}
	if (buffer) {
		av_free(buffer);
	}
	if (pFrameRGB) {
		av_frame_free(&pFrameRGB);
	}
	if (pFrame) {
		av_frame_free(&pFrame);
	}
	if (vs->video_ctx) {
		avcodec_free_context(&vs->video_ctx);
	}
	if (vs->format_ctx) {
		avformat_close_input(&vs->format_ctx);
	}

	return 0;
}
