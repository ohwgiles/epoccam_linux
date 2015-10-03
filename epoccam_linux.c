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
#define _GNU_SOURCE

#include <sys/select.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <gtk/gtk.h>

#ifndef PREFIX
#define PREFIX "/usr"
#endif
#define PATH_PIXMAPS PREFIX "/share/epoccam"
#define PATH_ICON_DEFAULT PATH_PIXMAPS "/icon_default.png"
#define PATH_ICON_AVAILABLE PATH_PIXMAPS "/icon_available.png"
#define PATH_ICON_RECORDING PATH_PIXMAPS "/icon_recording.png"
#define FFMPEG "ffmpeg"

// Ctrl-C handler
void signal_handler(int __UNUSED) {
    gtk_main_quit();
}

// Message constants
#define MAGIC_CONST			0xDEADC0DE
#define MESSAGE_VIDEOINFO		0x20000
#define MESSAGE_VIDEOHEADER		0x20001
#define MESSAGE_VIDEODATA		0x20002
#define MESSAGE_START_STREAMING		0x20003
#define MESSAGE_STOP_STREAMING		0x20004
#define MESSAGE_AUDIODATA		0x20004 // yes.
#define MESSAGE_KEEPALIVE		0x20005
#define VIDEO_TYPE_MPEG4                0
#define VIDEO_TYPE_H264                 1
#define VIDEO_TYPE_H264_FILE            2
#define AUDIO_TYPE_NO_AUDIO             0
#define AUDIO_TYPE_AAC                  1
#define AUDIO_TYPE_PCM                  2
#define AUDIO_TYPE_OPUS                 4
#define VIDEO_FLAG_UPSIDEDOWN           1
#define VIDEO_FLAG_KEYFRAME             2
#define VIDEO_FLAG_LITELIMIT            4
#define VIDEO_FLAG_HEADERS              8
#define VIDEO_FLAG_HORZ_FLIP            16

// AudioDataMsg flags
#define AUDIO_HEADERS                   1

#define VIDEO_SIZE_COUNT                35
#define AUDIO_FORMAT_COUNT              5

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
const char* audio_type_tostring(int type) {
    switch(type) {
    case AUDIO_TYPE_AAC:
        return "AAC";
    case AUDIO_TYPE_OPUS:
        return "OPUS";
    case AUDIO_TYPE_PCM:
        return "PCM";
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
            //exit(1);
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
void cb_clear(circular_buffer* cb) {
    cb->iread = cb->iwrite = 0;
}

int cb_empty(circular_buffer *cb) {
    return cb->iwrite == cb->iread;
}

void cb_init(circular_buffer *cb, int capacity) {
    cb->iread = 0;
    cb->iwrite = 0;
    cb->capacity = capacity + 1;
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
    int loop_tag;
} client_t;

// -------- Probe path to v4l loopback device

char* v4l2_probe() {
    // determine the device node by searching through sysfs
    const char path_sys[] = "/sys/devices/virtual/video4linux";
    char path_name[128];
    char dev_name[8];
    char* path = 0;

    DIR* dir = opendir(path_sys);
    if(!dir) return 0;

    struct dirent* dp;

    while((dp = readdir(dir))) {
        if(*dp->d_name == '.')
            continue;
        snprintf(path_name, 127, "%s/%s/name", path_sys, dp->d_name);
        FILE* f = fopen(path_name, "rb");
        if(!f)
            continue;
        fread(dev_name, 1, 8, f);
        fclose(f);
        if(strncmp(dev_name, "Loopback", 8) == 0) {
            path = malloc(strlen(dp->d_name)+2);
            sprintf(path, "/dev/%s", dp->d_name);
            return path;
        }
    }
    return 0;
}

// -------- Spawning child apps

typedef struct {
    int fd;
    pid_t pid;
} app_t ;

int app_start(app_t* a, char* const* argv) {
    int pfd[2];
    pipe(pfd);

    a->pid = fork();
    if(a->pid < 0) {
        return a->pid;
    } else if(a->pid == 0) {
        close(pfd[1]);
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        execvp(argv[0], argv);
        _exit(1);
    }

    a->fd = pfd[1];
    return 0;
}

void app_kill(app_t* a) {
    if(a->pid) {
    close(a->fd);
    kill(a->pid, SIGKILL);
    waitpid(a->pid, NULL, 0);
    a->pid = 0;
    }
}

// -------- Application routines

#define CIRCBUF_LEN 10240

typedef struct {
    server_t server;
    client_t clients[MAX_CLIENTS];
    int current_client;
    int streaming;
    circular_buffer pkt_audio;
    circular_buffer pkt_video;
    app_t proc_video;
    app_t proc_audio;
    char* v4l_device;
    GtkStatusIcon* icon;
} ec_t;

static void update_icon(ec_t* e) {
    if(e->streaming)
        gtk_status_icon_set_from_file(e->icon, PATH_ICON_RECORDING);
    else if(e->current_client != -1)
        gtk_status_icon_set_from_file(e->icon, PATH_ICON_AVAILABLE);
    else {
        gtk_status_icon_set_from_file(e->icon, PATH_ICON_DEFAULT);
        for(int i=0; i<MAX_CLIENTS; ++i) {
            if(e->clients[i].sd) {
                gtk_status_icon_set_from_file(e->icon, PATH_ICON_AVAILABLE);
                break;
            }
        }
    }
}

void ec_disconnect(ec_t* e, int client_index) {
    // stop streaming doesn't work so well. Even after sending the below
    // message, the client continues to send streaming packets. If we don't
    // handle them, we lose synchronisation with the message stream. Instead,
    // it's easier just to kick the client out.

//    msg_header_t stop_header = {
//        .version = MAGIC_CONST,
//        .type = MESSAGE_STOP_STREAMING,
//        .size = 0
//    };
//    send(e->clients[e->current_client].sd, &stop_header, sizeof(stop_header), 0);
//    gdk_input_remove(e->clients[e->current_client].loop_tag);
//    e->clients[e->current_client].loop_tag = 0;

    fprintf(stderr, "Client disconnected\n");
    if(client_index == e->current_client) {
        app_kill(&e->proc_video);
        app_kill(&e->proc_audio);

        e->streaming = 0;
        e->current_client = -1;
    }

    close(e->clients[client_index].sd);
    e->clients[client_index].sd = 0;
    if(e->clients[client_index].loop_tag)
        gtk_input_remove(e->clients[client_index].loop_tag);

    update_icon(e);
}


void ec_handle(ec_t* e, int client_index) {
    client_t* client = &e->clients[client_index];
    circular_buffer* video = &e->pkt_video;
    circular_buffer* audio = &e->pkt_audio;
    int which_video = 0;
    // If there is still pending video or audio payload, deal with that
    if(client->video_payload_left) {
        client->video_payload_left = cb_recv(video, client->sd, client->video_payload_left);
        // really here we should start listening for writable notifications on
        // the output pipe and do the write there, but practically the pipe
        // throughput will always be way higher than the network stream, so no
        // worries about blocking our network reads
        cb_write(&e->pkt_video, e->proc_video.fd, cb_count(video));
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
        cb_write(&e->pkt_audio, e->proc_audio.fd, cb_count(audio));
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
        fprintf(stderr, "Use the tray icon to start streaming\n");

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

void handle_client(gpointer data, gint src, GdkInputCondition cond) {
    ec_t* e = data;
    for(int i=0; i < MAX_CLIENTS; ++i) {
        if(e->clients[i].sd == src) {
            ec_handle(e, i);
            break;
        }
    }
}

void ec_start(ec_t* e, int client_index) {
    client_t* client = &e->clients[client_index];
    msg_header_t start_header = {
        .version = MAGIC_CONST,
        .type = MESSAGE_START_STREAMING,
        .size = sizeof(msg_start_t)
    };
    msg_start_t start_payload = {
        .video_idx = client->video_index,//which_video,
        .audio_idx = client->audio_index
    };
    send(client->sd, &start_header, sizeof(start_header), 0);
    send(client->sd, &start_payload, sizeof(start_payload), 0);
    e->current_client = client_index;
    e->streaming = 1;

    // TODO: pass correct codec types depending on available stream
    app_start(&e->proc_video, (char*[]){FFMPEG, "-an", "-vcodec", "h264", "-i", "-", "-f", "v4l2", e->v4l_device, NULL});
    if(client->audio_index >= 0)
        app_start(&e->proc_audio, (char*[]){FFMPEG, "-vn", "-acodec", "aac", "-i", "-", "-f", "alsa", "plughw:Loopback,1", NULL});

    update_icon(e);
}

void ec_join(ec_t* e) {
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        if(e->clients[i].sd == 0) {
            e->clients[i].sd = accept(e->server.sd, (struct sockaddr*)&e->clients[i].addr, &e->clients[i].socklen);
            e->clients[i].video_index = -1;
            e->clients[i].audio_index = -1;
            fcntl(e->clients[i].sd, F_SETFL, fcntl(e->clients[i].sd, F_GETFL) | O_NONBLOCK);
            e->clients[i].loop_tag = gdk_input_add(e->clients[i].sd, GDK_INPUT_READ, handle_client, e);
            if(e->current_client == -1)
                e->current_client = i;
            update_icon(e);
            break;
        }
    }
}
void handle_server(gpointer data, gint src, GdkInputCondition cond) {
    ec_join((ec_t*) data);
}



int ec_init(ec_t* e) {
    memset(e, 0, sizeof(ec_t));
    cb_init(&e->pkt_video, CIRCBUF_LEN);
    cb_init(&e->pkt_audio, CIRCBUF_LEN);

    if(server_init(&e->server))
        return -1;

    e->v4l_device = v4l2_probe();

    e->current_client = -1;
    e->streaming = 0;

    gdk_input_add(e->server.sd, GDK_INPUT_READ, handle_server, e);
    return 0;
}

void ec_cleanup(ec_t* e) {
    for(int i = 0; i < MAX_CLIENTS; ++i) {
        if(e->clients[i].sd) {
            ec_disconnect(e, i);
        }
    }
    close(e->server.sd);
    app_kill(&e->proc_video);
    app_kill(&e->proc_audio);
}

// -------- UI signal handlers

#define SETQ(o,s,v) g_object_set_qdata(G_OBJECT(o), g_quark_from_static_string(s), (gpointer)(uint64_t)(v))
#define GETQ(o,s) (int)(uint64_t)g_object_get_qdata(G_OBJECT(o), g_quark_from_static_string(s))

static gboolean menu_quit(GtkMenuItem* item, gpointer userdata) {
    fprintf(stderr, "QUIT\n");
    gtk_main_quit();
    return TRUE;
}
static gboolean menu_stop(GtkMenuItem* item, gpointer userdata) {
    ec_t* e = userdata;
    fprintf(stderr, "stop\n");
    ec_disconnect(e, GETQ(item, "client"));
    return TRUE;
}
static gboolean menu_start(GtkMenuItem* item, gpointer userdata) {
    ec_t* e = userdata;
    fprintf(stderr, "start\n");
    ec_start(e, e->current_client);
    return TRUE;
}
static gboolean menu_option(GtkMenuItem* item, gpointer userdata) {
    ec_t* e = userdata;
    e->current_client = GETQ(item, "client");
    e->clients[e->current_client].video_index = GETQ(item, "video");
    e->clients[e->current_client].audio_index = GETQ(item, "audio");
    menu_start(item, e);
    return TRUE;
}
static gboolean popup_menu(GtkStatusIcon* status_icon, guint button, guint activate_time, gpointer userdata)
{
    ec_t* e = userdata;
    GtkWidget* menu = gtk_menu_new();

    for(int i=0; i < MAX_CLIENTS; ++i) {
        client_t* c = &e->clients[i];
        if(c->sd != 0) {
            char* label;
            for(int j=0; j<c->device.video_count; ++j) {
                for(int k=c->device.audio_count ? 0 : -1; k<c->device.audio_count; ++k) {
                    asprintf(&label, "Client %d: %dx%d %s, %s", i, c->device.video[j].width,
                             c->device.video[j].height,video_type_tostring(c->device.video[j].type),
                             k == -1 ? "no audio" : audio_type_tostring(c->device.audio[k].type));
                    GtkWidget* checkitem = gtk_check_menu_item_new_with_label(label);
                    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(checkitem), c->video_index == j && c->audio_index == k);
                    gtk_widget_set_sensitive(checkitem, e->streaming == 0);
                    SETQ(checkitem, "client", i);
                    SETQ(checkitem, "video", j);
                    SETQ(checkitem, "audio", k);
                    g_signal_connect(G_OBJECT(checkitem), "activate", G_CALLBACK(menu_option), e);
                    gtk_widget_show(checkitem);
                    gtk_menu_shell_append(GTK_MENU_SHELL(menu), checkitem);
                    free(label);
                }
            }
            asprintf(&label, "Disconnect client %d", i);
            GtkWidget* stop = gtk_menu_item_new_with_label(label);
            SETQ(stop, "client", i);
            g_signal_connect(G_OBJECT(stop), "activate", G_CALLBACK(menu_stop), e);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), stop);
            gtk_widget_show(stop);
            free(label);
        }
    }

    GtkWidget* quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(G_OBJECT(quit), "activate", G_CALLBACK(menu_quit), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_widget_show(quit);

    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, status_icon, button, activate_time);
    g_object_ref_sink(menu);
    return TRUE;
}
// -------- main function, select loop

int main(int argc, char** argv) {
    ec_t e = { 0 };

    gtk_init(&argc, &argv);
    signal(SIGINT, signal_handler);

    if(system("lsmod|grep -q v4l2loopback") != 0) {
        fprintf(stderr, "Could not find v4l2loopback in lsmod, attempt to modprobe...\n");
        if(system("pkexec modprobe v4l2loopback")) {
            GtkWidget* dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "Could not load v4l2loopback kernel module");
            gtk_dialog_run (GTK_DIALOG (dialog));
            gtk_widget_destroy (dialog);
            return 1;
        }
    }

    if(system("lsmod|grep -q snd_aloop") != 0) {
        fprintf(stderr, "Could not find snd_aloop in lsmod, continuing anyway\n");
    }

    if(ec_init(&e))
        return -1;

    GtkStatusIcon* icon = gtk_status_icon_new_from_file(PATH_ICON_DEFAULT);
    gtk_status_icon_set_visible(icon, TRUE);
    g_signal_connect(icon, "popup-menu", G_CALLBACK(popup_menu), &e);
    e.icon = icon;
    update_icon(&e);

    gtk_main();

    ec_cleanup(&e);
    return 0;
}


