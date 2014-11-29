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
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>

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
    unsigned int audioFormatCount : 8;
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
    } audioFormat[AUDIO_FORMAT_COUNT];
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
	unsigned int payload_size;
} msg_video_t;


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

// -------- Client routines

typedef struct {
	int sd;
	struct sockaddr_in addr;
	socklen_t socklen;
	int payload_left;
} client_t;

int client_join(int server_sock, client_t* clients, int max_clients) {	
	for(int i = 0; i < MAX_CLIENTS; ++i) {
		if(clients[i].sd == 0) {
			clients[i].sd = accept(server_sock, (struct sockaddr*)&clients[i].addr, &clients[i].socklen);
			fcntl(clients[i].sd, F_SETFL, fcntl(clients[i].sd, F_GETFL) | O_NONBLOCK);
			return clients[i].sd + 1;
		}
	}
	return -1;
}

void client_disconnect(client_t* client) {
	fprintf(stderr, "Client disconnected\n");
	close(client->sd);
	client->sd = 0;
}

void client_handle_message(client_t* client, circular_buffer* buffer, int which_video) {
	// If there is still pending video payload, deal with that
	if(client->payload_left) {
		client->payload_left = cb_recv(buffer, client->sd, client->payload_left);
		return;
	}
	
	// Interpret a new message
	msg_header_t hdr;
	int n = recv(client->sd, &hdr, sizeof(hdr), MSG_WAITALL);
	if(n == 0)
		return client_disconnect(client);
	if(n <= 0)
		return perror("recv1");
	
	if(hdr.type == MESSAGE_KEEPALIVE && hdr.size) {
		msg_device_t device;
		n = recv(client->sd, &device, sizeof(msg_device_t), MSG_WAITALL);
		if(n != sizeof(msg_device_t))
			return perror("recv2");
			
		fprintf(stderr, "Received %d video sizes\n", device.video_count);
		for(int j = 0; j < device.video_count; ++j) {
			fprintf(stderr, "Offered video size: %dx%d %s at %ffps\n", 
				device.video[j].width, device.video[j].height,
				video_type_tostring(device.video[j].type),
				device.video[j].fps);
		}
		if(which_video >= device.video_count)
			which_video = device.video_count - 1;

		fprintf(stderr, "Selection option %d\n", which_video);
		
		// Send start message. TODO: make this externally triggerable
		msg_header_t start_header = {
			.version = MAGIC_CONST,
			.type = MESSAGE_START_STREAMING,
			.size = sizeof(msg_start_t)
		};
		msg_start_t start_payload = {
			.video_idx = which_video,
			.audio_idx = 0xffffffff
		};
		send(client->sd, &start_header, sizeof(start_header), 0);
		send(client->sd, &start_payload, sizeof(start_payload), 0);
	} else if(hdr.type == MESSAGE_VIDEODATA) {
		msg_video_t vid;
		n = recv(client->sd, &vid, sizeof(msg_video_t), 0);
		client->payload_left = vid.payload_size;
	} else if(hdr.type != MESSAGE_KEEPALIVE) {
		fprintf(stderr, "unknown message type %x\n", hdr.type);
	}
	
}

// -------- main function, select loop

#define BUFFER_LEN 10240

int main(int argc, char** argv) {
	server_t server;
	client_t clients[MAX_CLIENTS] = {{0}};
	circular_buffer cb;
	int max_fd;
	struct timeval tv;
	fd_set read_fds;
	fd_set write_fds;
	int requested_video_index;
	
	requested_video_index = 0;
	if(argc > 1)
		requested_video_index = atoi(argv[1]);

	signal(SIGINT, signal_handler);

	if(server_init(&server))
		return -1;
	max_fd = server.sd + 1;
	
	cb_new(&cb, BUFFER_LEN);
	fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
	
	while(!quit) {
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		for(int i = 0; i < MAX_CLIENTS; ++i) {
			if(clients[i].sd) {
				FD_SET(clients[i].sd, &read_fds);
			}
		}
		FD_SET(server.sd, &read_fds);
		if(!cb_empty(&cb))
			FD_SET(1, &write_fds);
			
		int r = select(max_fd, &read_fds, &write_fds, NULL, &tv);
		if(r < 0)
			break;
		
		// Handle new clients
		if(FD_ISSET(server.sd, &read_fds)) {
			fprintf(stderr, "New client connected\n");
			max_fd = client_join(server.sd, clients, MAX_CLIENTS);
		}
		// Handle existing clients
		for(int i = 0; i < MAX_CLIENTS; ++i) {
			if(clients[i].sd && FD_ISSET(clients[i].sd, &read_fds)) {
				client_handle_message(&clients[i], &cb, requested_video_index);
			}
		}
		// Write out the output buffers
		if(FD_ISSET(1, &write_fds)) {
			cb_write(&cb, 1, cb_count(&cb));
		}
	}
	// Cleanup
	for(int i = 0; i < MAX_CLIENTS; ++i) {
		if(clients[i].sd) {
			client_disconnect(&clients[i]);
		}
	}
	close(server.sd);
	return 0;
}


