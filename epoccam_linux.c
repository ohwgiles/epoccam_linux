/**
 * epoccam_linux
 * C program to communicate with Kinoni's EpocCam mobile application
 * Copyright (c) 2014, Oliver Giles
 * 
 * Please see README for usage instructions
 * 
 * EpocCam is produced by Kinoni (http://www.kinoni.com), who generously
 * provided useful documentation and support for this project.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer. 
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */
 
#define _POSIX_C_SOURCE 1
#include <sys/select.h>

#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
//#include <sys/select.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <sys/stat.h>
#include <stropts.h>

#include <linux/videodev2.h>
#include <alsa/asoundlib.h>




// Ctrl-C handler
static volatile int quit = 0;
void signal_handler(int __UNUSED) {
    quit = 1;
}

// Message constants
#define MAGIC_CONST					0xDEADC0DE
#define MESSAGE_VIDEOINFO			0x20000
#define MESSAGE_VIDEOHEADER			0x20001
#define MESSAGE_VIDEODATA			0x20002
#define MESSAGE_START_STREAMING		0x20003
#define MESSAGE_STOP_STREAMING		0x20004
#define MESSAGE_AUDIODATA			0x20004 // yes.
#define MESSAGE_KEEPALIVE			0x20005
#define VIDEO_TYPE_MPEG4      0
#define VIDEO_TYPE_H264       1
#define VIDEO_TYPE_H264_FILE  2
#define AUDIO_TYPE_NO_AUDIO   0
#define AUDIO_TYPE_AAC        1
#define AUDIO_TYPE_PCM        2
#define AUDIO_TYPE_OPUS       4
#define VIDEO_FLAG_UPSIDEDOWN 1
#define VIDEO_FLAG_KEYFRAME   2
#define VIDEO_FLAG_LITELIMIT  4
#define VIDEO_FLAG_HEADERS    8
#define VIDEO_FLAG_HORZ_FLIP 16

// AudioDataMsg flags
#define AUDIO_HEADERS    1

#define VIDEO_SIZE_COUNT 35
#define AUDIO_FORMAT_COUNT 5

// Message types
typedef struct {
	unsigned int version;
	unsigned int __UNUSED1;
	unsigned int type;
	unsigned int size;
} msg_header_t;

typedef struct {
	unsigned int software_id;
	unsigned int machine_id;
	unsigned int __UNUSED1 : 24;
    unsigned int audio_count : 8;
	unsigned int video_count;
	struct {
		unsigned int width : 12;
		unsigned int height : 12;
		unsigned int type : 8;
		float fps;
	} video[VIDEO_SIZE_COUNT];
    struct {
        unsigned int type : 8;
        unsigned int __UNUSED2 : 24;
        unsigned int __UNUSED3;
    } audio[AUDIO_FORMAT_COUNT];
} msg_device_t;

typedef struct {
	unsigned int video_idx;
	unsigned int audio_idx;
    unsigned short __UNUSED1;
    unsigned short __UNUSED2;
} msg_start_t;

typedef struct {
	unsigned int flags;
	unsigned int timestamp;
	unsigned int size;
} msg_payload_t;


// Pretty-print video format helper
const char* video_type_tostring(int type) {
	switch(type) {
	case VIDEO_TYPE_MPEG4:
		return "MPEG4";
	case VIDEO_TYPE_H264:
		return "H264";
	default:
		return "Unknown";
	}
}

// -------- Circular buffer

typedef struct {
    int  iread;
    int  iwrite;
    int  capacity;
    char *ptr;
} circular_buffer;

int cb_recv(circular_buffer *cb, int fd, int n) {
	while(n) {
		int z = cb->capacity - cb->iwrite;
		z = z < n ? z : n;
		int r = read(fd, &cb->ptr[cb->iwrite], z);
		if(r <= 0) {
			if(errno != EWOULDBLOCK)
				perror("cb_recv");
			return n;
		}
		n -= r;
		int overwrite = (cb->iwrite < cb->iread && cb->iwrite + r >= cb->iread);
		cb->iwrite = (cb->iwrite + r) % cb->capacity;
		if(overwrite) {
			fprintf(stderr, "Buffer overflow, packets dropped\n");
			cb->iread = (cb->iwrite + 1) % cb->capacity;
			exit(1);
		}
	}
	return n;
}

int cb_write(circular_buffer* cb, int fd, int n) {
	while(n) {
		int z = cb->capacity - cb->iread;
		z = z < n ? z : n;
		int r = write(fd, &cb->ptr[cb->iread], z);
		if(r <= 0) {
			if(errno != EWOULDBLOCK)
				perror("cb_write");
			return n;
		}
		n -= r;
		cb->iread = (cb->iread + r) % cb->capacity;
	}
	return n;
}

void cb_move(circular_buffer* cb, unsigned char* output, int n) {
	while(n) {
		int z = cb->capacity - cb->iread;
		z = z < n ? z : n;
		memmove(output, &cb->ptr[cb->iread], z);
		n -= z;
		output += z;
		cb->iread = (cb->iread + z) % cb->capacity;
	}
}

int cb_count(circular_buffer* cb) {
	return cb->iread > cb->iwrite ?
		cb->capacity - cb->iread + cb->iwrite :
		cb->iwrite - cb->iread;
}

int cb_available(circular_buffer* cb) {
	return cb->iread > cb->iwrite ?
		cb->iread - cb->iwrite - 1 :
		cb->capacity - cb->iwrite + cb->iread - 1;
}

int cb_full(circular_buffer *cb) {
    return (cb->iwrite + 1) % cb->capacity == cb->iread;
}

int cb_empty(circular_buffer *cb) {
    return cb->iwrite == cb->iread;
}

void cb_new(circular_buffer *cb, int capacity) {
	cb->iread = 0;
    cb->iwrite   = 0;
    cb->capacity  = capacity + 1;
    cb->ptr = (char*) malloc(cb->capacity);
}

void cb_delete(circular_buffer *cb) {
    free(cb->ptr);
}


// -------- Server routines

#define MAX_CLIENTS 3

typedef struct {
	int sd;
	struct sockaddr_in addr;
} server_t;

int server_init(server_t* s) {
	memset(&s->addr, 0, sizeof(struct sockaddr_in));
	s->addr.sin_family = AF_INET;
	s->addr.sin_addr.s_addr = htonl(INADDR_ANY);
	s->addr.sin_port = htons(5055);
	
	s->sd = socket(AF_INET, SOCK_STREAM, 0);
	if(s->sd < 0)
		return perror("socket"), -1;
		
	if(bind(s->sd, (struct sockaddr*)&s->addr, sizeof(struct sockaddr_in)) < 0)
		return perror("bind"), -1;
		
	if(listen(s->sd, MAX_CLIENTS) < 0)
		return perror("listen"), -1;
		
	return 0;
}

// -------- Client structure

typedef struct {
	int sd;
	struct sockaddr_in addr;
	socklen_t socklen;
	int video_index;
	int video_payload_left;
	int audio_index;
	int audio_payload_left;
	msg_device_t device;
} client_t;

// -------- Video decoding routines

#define VIDEO_INBUF_SIZE 204800
FILE* outfile;
typedef struct {
	AVCodec* codec;
	AVFormatContext* fmt;
    AVCodecContext* ctx;
    AVFrame* frame;
	AVPacket pkt;
	uint8_t* rawdata[4];
	int bufsize;
	int linesizes[4];
	uint8_t stream_buffer[VIDEO_INBUF_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];
	int bytes_buffered;
	int np;
	int prepared;
	uint8_t* outbuf;
	int outbuf_size;
} h264_t;

int h264_init(h264_t* h264, int (*reader)(void*,uint8_t*,int), void* udata) {
    av_register_all();
	avcodec_register_all();
	av_init_packet(&h264->pkt);
	h264->pkt.data = NULL;
	h264->pkt.size = 0;
	
	uint8_t* tmpbuf = (uint8_t*) malloc(4096);
	
	
	
    h264->fmt = avformat_alloc_context();
    h264->fmt->pb = avio_alloc_context(tmpbuf, 4096, 0, udata, reader, NULL, NULL);
    
    h264->codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    h264->ctx = avcodec_alloc_context3(h264->codec);
    avcodec_open2(h264->ctx, h264->codec, NULL);

h264->prepared = 0;
    h264->bytes_buffered = 0;
  
	h264->outbuf = 0;
	h264->outbuf_size = 0;
	h264->np = 0;
	outfile = fopen("eeviddec", "wb");
	return 0;
}
void h264_prepare(h264_t* h264, int width, int height) {
	int ret = avformat_open_input(&h264->fmt, "tt", NULL, NULL);
	if(ret != 0) {
		fprintf(stderr, "error opening fmt (%s)\n", av_err2str(ret));
		exit(1);
	}

	fprintf(stderr, "avformate_open_input returned %d\n", ret);
		h264->frame = av_frame_alloc();

	h264->bufsize = av_image_alloc(h264->rawdata, h264->linesizes, width, height, AV_PIX_FMT_YUV420P, 1);
	fprintf(stderr, "buffer size: %d\n", h264->bufsize);
	h264->prepared = 1;
}

int h264_feed(void* b, uint8_t* out, int sz) {
	if(sz > cb_count(b))
		sz = cb_count(b);
	cb_move(b, out, sz);
	return sz;
}

int h264_decode(h264_t* h264, int* got_frame) {
	return avcodec_decode_video2(h264->ctx, h264->frame, got_frame, &h264->pkt);
}
int h264_nframes(h264_t* h264) {
	int sz = av_samples_get_buffer_size(NULL, h264->ctx->channels,
		h264->frame->nb_samples, h264->ctx->sample_fmt, 1);
	return sz / av_get_bytes_per_sample(h264->ctx->sample_fmt);
}
void h264_cleanup(h264_t* h264) {
	avcodec_close(h264->ctx);
    av_free(h264->ctx);
    av_free(h264->frame);
}

// -------- Audio decoding routines

#define AUDIO_INBUF_SIZE 204800

typedef struct {
	AVCodec* codec;
    AVCodecContext* ctx;
    AVFrame* frame;
	AVPacket pkt;
	uint8_t pcm_buffer[AUDIO_INBUF_SIZE + FF_INPUT_BUFFER_PADDING_SIZE];
} aac_t;

int aac_init(aac_t* aac) {
	avcodec_register_all();
	av_init_packet(&aac->pkt);
    aac->codec = avcodec_find_decoder(CODEC_ID_AAC);
    aac->ctx = avcodec_alloc_context3(aac->codec);
    if(avcodec_open2(aac->ctx, aac->codec, NULL) < 0) {
        fprintf(stderr, "could not open codec\n");
        return -1;
    }
	aac->frame = av_frame_alloc();
	aac->pkt.data = aac->pcm_buffer;
	aac->pkt.size = 0;
	return 0;
}
int aac_decode(aac_t* aac, int* got_frame) {
	return avcodec_decode_audio4(aac->ctx, aac->frame, got_frame, &aac->pkt);
}
int aac_nframes(aac_t* aac) {
	int sz = av_samples_get_buffer_size(NULL, aac->ctx->channels,
		aac->frame->nb_samples, aac->ctx->sample_fmt, 1);
	return sz / av_get_bytes_per_sample(aac->ctx->sample_fmt);
}
void aac_cleanup(aac_t* aac) {
	avcodec_close(aac->ctx);
    av_free(aac->ctx);
    av_free(aac->frame);
}

// -------- Sound output routines

typedef struct {
	int open;
	int failed;
	snd_pcm_t* pcm;
} snd_t;

int snd_init(snd_t* s, const char* device, int rate, int format) {
	int err;
	s->open = 0;
	s->failed = 1;
	if((err = snd_pcm_open(&s->pcm, device, SND_PCM_STREAM_PLAYBACK, 0)) < 0)
		return fprintf(stderr, "cannot open audio device %s (%s)\n", device, snd_strerror(err));
	snd_pcm_hw_params_t *hw_params = 0;
	if((err = snd_pcm_hw_params_malloc(&hw_params)) < 0)
		return fprintf(stderr, "cannot allocate hardware parameter structure (%s)\n", snd_strerror(err));
	// any failure down here causes a leak of hw_params;
	if((err = snd_pcm_hw_params_any(s->pcm, hw_params)) < 0)
		return fprintf(stderr, "cannot initialize hardware parameter structure (%s)\n", snd_strerror(err)), err;
	if((err = snd_pcm_hw_params_set_channels(s->pcm, hw_params, 1)) < 0)
		return fprintf(stderr, "cannot set channel count (%s)\n", snd_strerror(err)), err;
	if((err = snd_pcm_hw_params_set_access(s->pcm, hw_params, SND_PCM_ACCESS_RW_NONINTERLEAVED)) < 0)
		return fprintf(stderr, "cannot set access type (%s)\n", snd_strerror(err)), err;
	if((err = snd_pcm_hw_params_set_format(s->pcm, hw_params, format)) < 0)
		return fprintf(stderr, "cannot set sample format (%s)\n", snd_strerror(err)), err;
	if((err = snd_pcm_hw_params_set_rate(s->pcm, hw_params, rate, 0)) < 0)
		return fprintf(stderr, "cannot set sample rate (%s)\n", snd_strerror(err)), err;
	if((err = snd_pcm_hw_params(s->pcm, hw_params)) < 0)
		return fprintf(stderr, "cannot set parameters (%s)\n", snd_strerror(err)), err;
	snd_pcm_hw_params_free(hw_params);
	if((err = snd_pcm_prepare (s->pcm)) < 0)
		return fprintf(stderr, "cannot prepare audio interface for use (%s)\n", snd_strerror(err)), err;
	s->failed = 0;
	s->open = 1;
	return 0;
}

int snd_format_from_libav(int fmt) {
	switch(fmt) {
	case AV_SAMPLE_FMT_FLT:
	case AV_SAMPLE_FMT_FLTP:
		return SND_PCM_FORMAT_FLOAT_LE;
	case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S16P:
		return SND_PCM_FORMAT_S16;
	case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
		return SND_PCM_FORMAT_S32;
	}
	return -1;
}

void snd_close(snd_t* s) {
	s->open = 0;
	s->failed = 0;
	snd_pcm_close(s->pcm);
}

// -------- Video output routines

typedef struct {
	int fd;
} v4l2_t;

void v4l2_init(v4l2_t* v) {
	v->fd = open("/dev/video0", O_WRONLY);
	struct v4l2_capability caps;
	ioctl(v->fd, VIDIOC_QUERYCAP, &caps);
	fprintf(stderr, "driver: %s\n", caps.driver);

}

void v4l2_setup(v4l2_t* v, int width, int height) {
	struct v4l2_format fmt;
	ioctl(v->fd, VIDIOC_G_FMT, &fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	fmt.fmt.pix.width = width;
	fmt.fmt.pix.height = height;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
	ioctl(v->fd, VIDIOC_S_FMT, &fmt);
}

void v4l2_close(v4l2_t* v) {
	close(v->fd);
}

// -------- Application routines

#define CIRCBUF_LEN 204800

enum {
	PFD_STDOUT = 0,
	PFD_SERVER,
	PFD_CLIENTS,
	PFD_N = PFD_CLIENTS + MAX_CLIENTS
};

typedef struct {
	server_t server;
	client_t clients[MAX_CLIENTS];
	int active_client;
	circular_buffer pkt_audio;
	circular_buffer pkt_video;
	aac_t aac;
	h264_t h264;
	v4l2_t v4l;
	snd_t snd;
	struct pollfd pfds[PFD_N];
} ec_t;

int ec_init(ec_t* e) {
	memset(e, 0, sizeof(ec_t));
	cb_new(&e->pkt_video, CIRCBUF_LEN);
	cb_new(&e->pkt_audio, CIRCBUF_LEN);
	
	if(h264_init(&e->h264, h264_feed, &e->pkt_video))
		return -1;
	if(aac_init(&e->aac))
		return -1;
	if(server_init(&e->server))
		return -1;
	
	v4l2_init(&e->v4l);


	e->active_client = -1;
	e->pfds[PFD_STDOUT].fd = 1;
	e->pfds[PFD_STDOUT].events = 0;
	e->pfds[PFD_SERVER].fd = e->server.sd;
	e->pfds[PFD_SERVER].events = POLLIN;
	//	fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);

	return 0;
}

void ec_join(ec_t* e) {
	for(int i = 0; i < MAX_CLIENTS; ++i) {
		if(e->clients[i].sd == 0) {
			e->clients[i].sd = accept(e->server.sd, (struct sockaddr*)&e->clients[i].addr, &e->clients[i].socklen);
			e->clients[i].video_index = 0;
			e->clients[i].audio_index = 0;
			fcntl(e->clients[i].sd, F_SETFL, fcntl(e->clients[i].sd, F_GETFL) | O_NONBLOCK);
			e->pfds[PFD_CLIENTS + i].fd = e->clients[i].sd;
			e->pfds[PFD_CLIENTS + i].events = POLLIN;
			break;
		}
	}
}

void ec_process_video(ec_t* e) {
	if(cb_count(&e->pkt_video) < 80960) return;
	if(!e->h264.prepared) {// move prepared up
		client_t* c = &e->clients[e->active_client];
		int w = c->device.video[c->video_index].width;
		int h = c->device.video[c->video_index].height;
		fprintf(stderr, "%dx%d\n", w, h);
		h264_prepare(&e->h264, w, h);
		v4l2_setup(&e->v4l, w, h);
	}
	//fprintf(stderr, "processing\n");
	int got_frame = 0;
	        AVPacket orig_pkt = e->h264.pkt;

	if(av_read_frame(e->h264.fmt, &e->h264.pkt) >= 0) {
		if(h264_decode(&e->h264, &got_frame) < 0)
			return;
		
        if (got_frame) {
			//fprintf(stderr, "got frame\n");
            av_image_copy(e->h264.rawdata, e->h264.linesizes,
                          (const uint8_t **)(e->h264.frame->data), e->h264.frame->linesize,
                          e->h264.ctx->pix_fmt, e->h264.ctx->width, e->h264.ctx->height);
            write(e->v4l.fd, e->h264.rawdata[0], e->h264.bufsize);
            //fprintf(stderr, "writing %d bytes to file\n", e->h264.bufsize);
        };
        e->h264.pkt.size = 0;
        av_free_packet(&orig_pkt);
    }
}

void ec_process_audio(ec_t* e) {
	int z = cb_count(&e->pkt_audio);
	if(!z) return;
	if(z + (e->aac.pkt.data - e->aac.pcm_buffer) > AUDIO_INBUF_SIZE)
		z = AUDIO_INBUF_SIZE - (e->aac.pkt.data - e->aac.pcm_buffer);
	int buffer_offset = e->aac.pkt.data - e->aac.pcm_buffer;

	//fprintf(stderr, "moving %d bytes of available %d to buffer at offest %d\n", z, cb_count(&e.pkt_audio), buffer_offset);
	
	cb_move(&e->pkt_audio, e->aac.pkt.data, z);
	e->aac.pkt.size += z;
	//fprintf(stderr, "first byte: %x\n", av->pkt.data[0]);
	
	
	int got_frame = 0;
	int len = aac_decode(&e->aac, &got_frame);
	if(len < 0) {
		fprintf(stderr, "decode error\n");
		return;
	}
	
	if(got_frame) {
		if(!e->snd.open && !e->snd.failed) {
			snd_init(&e->snd, "plughw:2,0", e->aac.ctx->sample_rate, snd_format_from_libav(e->aac.ctx->sample_fmt));
			fprintf(stderr, "init'd sound. pdsize: %d\n", snd_pcm_poll_descriptors_count(e->snd.pcm));
			//snd_pcm_poll_descriptors(snd.pcm, &pfds[PFD_ALSA], 1);
		}
		
		
		if(!e->snd.failed) {
			int err;
			int j = aac_nframes(&e->aac);
			void* bufs[2] = { e->aac.frame->data[0], NULL };
			//fprintf(stderr, "write to snd\n");
			if((err = snd_pcm_writen(e->snd.pcm, bufs, j)) != j) {
				 fprintf(stderr, "write to audio interface failed (%s)\n", snd_strerror(err));
				 snd_pcm_recover(e->snd.pcm, err, 0);
			}
		}
	}
	e->aac.pkt.size -= len;
	e->aac.pkt.data += len;
	if(buffer_offset > 1024) {
		//fprintf(stderr, "tidying buffer\n");
		memmove(e->aac.pcm_buffer, e->aac.pkt.data, e->aac.pkt.size);
		e->aac.pkt.data = e->aac.pcm_buffer;
	}
}

void ec_disconnect(ec_t* e, int client_index) {
	fprintf(stderr, "Client disconnected\n");
	close(e->clients[client_index].sd);
	e->clients[client_index].sd = 0;
	e->pfds[PFD_CLIENTS + client_index].fd = 0;
	e->pfds[PFD_CLIENTS + client_index].events = 0;
	// todo only if this is the active client
	snd_close(&e->snd);
	e->active_client = -1;
}

void ec_handle(ec_t* e, int client_index) {
	client_t* client = &e->clients[client_index];
	circular_buffer* video = &e->pkt_video;
	circular_buffer* audio = &e->pkt_audio;
	int which_video = 0;
	// If there is still pending video or audio payload, deal with that
	if(client->video_payload_left) {
		client->video_payload_left = cb_recv(video, client->sd, client->video_payload_left);
		return;
	}
	if(client->audio_payload_left) {
		if(client->audio_payload_left == 2) {
			fprintf(stderr, "consuming odd bytes\n");
		char _[2];
		read(client->sd, _, 2);
		client->audio_payload_left -= 2;
		}
		client->audio_payload_left = cb_recv(audio, client->sd, client->audio_payload_left);
		return;
	}

	// Interpret a new message
	msg_header_t hdr;
	int n = recv(client->sd, &hdr, sizeof(hdr), MSG_WAITALL);
	if(n == 0)
		return ec_disconnect(e, client_index);
	if(n <= 0)
		return perror("recv1");
	
	if(hdr.type == MESSAGE_KEEPALIVE && hdr.size) {
		msg_device_t* device = &client->device;
		n = recv(client->sd, device, sizeof(msg_device_t), MSG_WAITALL);
		if(n != sizeof(msg_device_t))
			return perror("recv2");
			
		fprintf(stderr, "Received %d video sizes\n", device->video_count);
		for(int j = 0; j < device->video_count; ++j) {
			fprintf(stderr, "Offered video size: %dx%d %s at %ffps\n", 
				device->video[j].width, device->video[j].height,
				video_type_tostring(device->video[j].type),
				device->video[j].fps);
		}
		if(which_video >= device->video_count)
			which_video = device->video_count - 1;

		fprintf(stderr, "Selection option %d\n", which_video);
		fprintf(stderr, "Received %d audio options\n", device->audio_count);
		for(int j = 0; j < device->audio_count; ++j) {
			fprintf(stderr, "Offered audio type: %d\n", device->audio[j].type);
		}
		//exit(2);
e->h264.prepared = 0;
		// Send start message. TODO: make this externally triggerable
		msg_header_t start_header = {
			.version = MAGIC_CONST,
			.type = MESSAGE_START_STREAMING,
			.size = sizeof(msg_start_t)
		};
		msg_start_t start_payload = {
			.video_idx = 0,//which_video,
			.audio_idx = 0
		};
		send(client->sd, &start_header, sizeof(start_header), 0);
		send(client->sd, &start_payload, sizeof(start_payload), 0);
		e->active_client = client_index;
	} else if(hdr.type == MESSAGE_VIDEODATA) {
		msg_payload_t vid;
		n = recv(client->sd, &vid, sizeof(msg_payload_t), MSG_WAITALL);
		client->video_payload_left = vid.size;
	} else if(hdr.type == MESSAGE_AUDIODATA) {
		msg_payload_t snd;
		n = recv(client->sd, &snd, sizeof(msg_payload_t), MSG_WAITALL);
		client->audio_payload_left = snd.size;		
	} else if(hdr.type != MESSAGE_KEEPALIVE) {
		fprintf(stderr, "unknown message type %x\n", hdr.type);
	}
}

void ec_cleanup(ec_t* e) {
	// Cleanup
	if(e->snd.open)
		snd_close(&e->snd);
	aac_cleanup(&e->aac);
	for(int i = 0; i < MAX_CLIENTS; ++i) {
		if(e->clients[i].sd) {
			ec_disconnect(e, i);
		}
	}
	close(e->server.sd);
}

// -------- main function, select loop

int main(int argc, char** argv) {
	ec_t e;
	int audio_debug = 0;
	int video_debug = 0;
	
	signal(SIGINT, signal_handler);
	
	if(ec_init(&e))
		return -1;
		
	while(!quit) {
		if(audio_debug && !cb_empty(&e.pkt_audio))
			e.pfds[PFD_STDOUT].events = POLLIN;
		
		int r = poll(e.pfds, PFD_N, 1000);
		if(r < 0)
			break;
		
		// Handle new clients
		if(e.pfds[PFD_SERVER].revents & POLLIN) {
			ec_join(&e);
		}
		// Handle existing clients
		for(int i = 0; i < MAX_CLIENTS; ++i) {
			if(e.clients[i].sd &&
				(e.pfds[PFD_CLIENTS+i].revents & POLLIN))
			{
				ec_handle(&e, i);
			}
		}
		// Write out the output buffers
		if(audio_debug) {
			if(e.pfds[PFD_STDOUT].revents & POLLIN)
				cb_write(&e.pkt_audio, 1, cb_count(&e.pkt_audio));
		} else if(video_debug) {
			//if(e.pfds[PFD_STDOUT].revents & POLLIN)
				cb_write(&e.pkt_video, 1, cb_count(&e.pkt_video));
		} else {
			if(e.active_client > -1) {
				ec_process_video(&e);
				//ec_process_audio(&e);
			}
		}
	}
	ec_cleanup(&e);
	return 0;
}


