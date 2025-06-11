#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/select.h>
#include <linux/videodev2.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>

#define BUFFER_COUNT 4
#define RECORDING_DURATION 30  // 30 segundos
#define TARGET_FPS 25

struct buffer {
    void *start;
    size_t length;
};

struct camera {
    int fd;
    struct buffer *buffers;
    unsigned int n_buffers;
    int width;
    int height;
    int format;
};

struct encoder {
    AVFormatContext *fmt_ctx;
    AVCodecContext *codec_ctx;
    AVStream *video_stream;
    AVFrame *frame;
    AVPacket *pkt;
    struct SwsContext *sws_ctx;
    int64_t frame_count;
};

volatile int keep_recording = 1;

void signal_handler(int sig) {
    if (sig == SIGINT) {
        printf("\nInterrompendo gravação...\n");
        keep_recording = 0;
    }
}

// Converter YUYV para YUV420P (formato usado pelo H.264)
void yuyv_to_yuv420p(const unsigned char *yuyv, AVFrame *frame, int width, int height) {
    const unsigned char *src = yuyv;
    unsigned char *y_plane = frame->data[0];
    unsigned char *u_plane = frame->data[1];
    unsigned char *v_plane = frame->data[2];

    // Converter Y (luminância)
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j += 2) {
            y_plane[i * frame->linesize[0] + j] = src[0];     // Y1
            y_plane[i * frame->linesize[0] + j + 1] = src[2]; // Y2
            src += 4;
        }
    }

    // Converter U e V (crominância) - subsample 4:2:0
    src = yuyv;
    for (int i = 0; i < height; i += 2) {
        for (int j = 0; j < width; j += 4) {
            int u1 = src[1];
            int v1 = src[3];
            int u2 = src[width * 2 + 1];
            int v2 = src[width * 2 + 3];

            // Média dos valores U e V para subsample
            u_plane[(i/2) * frame->linesize[1] + (j/2)] = (u1 + u2) / 2;
            v_plane[(i/2) * frame->linesize[2] + (j/2)] = (v1 + v2) / 2;

            src += 8;
        }
        src += width * 2; // Pular linha par
    }
}

// Inicializar encoder FFmpeg
int init_encoder(struct encoder *enc, const char *filename, int width, int height, int fps) {
    int ret;

    // Alocar contexto de formato
    avformat_alloc_output_context2(&enc->fmt_ctx, NULL, NULL, filename);
    if (!enc->fmt_ctx) {
        fprintf(stderr, "Erro ao criar contexto de saída\n");
        return -1;
    }

    // Encontrar codec H.264
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "Codec H.264 não encontrado\n");
        return -1;
    }

    // Adicionar stream de vídeo
    enc->video_stream = avformat_new_stream(enc->fmt_ctx, NULL);
    if (!enc->video_stream) {
        fprintf(stderr, "Erro ao criar stream de vídeo\n");
        return -1;
    }

    // Alocar contexto do codec
    enc->codec_ctx = avcodec_alloc_context3(codec);
    if (!enc->codec_ctx) {
        fprintf(stderr, "Erro ao alocar contexto do codec\n");
        return -1;
    }

    // Configurar parâmetros do codec
    enc->codec_ctx->width = width;
    enc->codec_ctx->height = height;
    enc->codec_ctx->time_base = (AVRational){1, fps};
    enc->codec_ctx->framerate = (AVRational){fps, 1};
    enc->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->codec_ctx->bit_rate = 2000000; // 2 Mbps
    enc->codec_ctx->gop_size = fps;     // Um keyframe a cada segundo
    enc->codec_ctx->max_b_frames = 1;

    // Configurações específicas para H.264
    if (codec->id == AV_CODEC_ID_H264) {
        av_opt_set(enc->codec_ctx->priv_data, "preset", "fast", 0);
        av_opt_set(enc->codec_ctx->priv_data, "tune", "zerolatency", 0);
    }

    // Se formato requer header global
    if (enc->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        enc->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Abrir codec
    ret = avcodec_open2(enc->codec_ctx, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Erro ao abrir codec: %s\n", av_err2str(ret));
        return -1;
    }

    // Copiar parâmetros para o stream
    ret = avcodec_parameters_from_context(enc->video_stream->codecpar, enc->codec_ctx);
    if (ret < 0) {
        fprintf(stderr, "Erro ao copiar parâmetros do codec\n");
        return -1;
    }

    enc->video_stream->time_base = enc->codec_ctx->time_base;

    // Alocar frame
    enc->frame = av_frame_alloc();
    if (!enc->frame) {
        fprintf(stderr, "Erro ao alocar frame\n");
        return -1;
    }

    enc->frame->format = enc->codec_ctx->pix_fmt;
    enc->frame->width = enc->codec_ctx->width;
    enc->frame->height = enc->codec_ctx->height;

    ret = av_frame_get_buffer(enc->frame, 32);
    if (ret < 0) {
        fprintf(stderr, "Erro ao alocar buffer do frame\n");
        return -1;
    }

    // Alocar packet
    enc->pkt = av_packet_alloc();
    if (!enc->pkt) {
        fprintf(stderr, "Erro ao alocar packet\n");
        return -1;
    }

    // Abrir arquivo de saída
    if (!(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&enc->fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Erro ao abrir arquivo de saída: %s\n", av_err2str(ret));
            return -1;
        }
    }

    // Escrever header
    ret = avformat_write_header(enc->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Erro ao escrever header: %s\n", av_err2str(ret));
        return -1;
    }

    enc->frame_count = 0;

    printf("Encoder inicializado: %s (%dx%d @ %d fps)\n", filename, width, height, fps);
    return 0;
}

// Codificar e escrever frame
int encode_frame(struct encoder *enc, const unsigned char *yuyv_data) {
    int ret;

    // Converter YUYV para YUV420P
    yuyv_to_yuv420p(yuyv_data, enc->frame, enc->codec_ctx->width, enc->codec_ctx->height);

    // Definir PTS do frame
    enc->frame->pts = enc->frame_count++;

    // Enviar frame para encoder
    ret = avcodec_send_frame(enc->codec_ctx, enc->frame);
    if (ret < 0) {
        fprintf(stderr, "Erro ao enviar frame para encoder: %s\n", av_err2str(ret));
        return -1;
    }

    // Receber packets codificados
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc->codec_ctx, enc->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Erro ao receber packet: %s\n", av_err2str(ret));
            return -1;
        }

        // Ajustar timestamps
        av_packet_rescale_ts(enc->pkt, enc->codec_ctx->time_base, enc->video_stream->time_base);
        enc->pkt->stream_index = enc->video_stream->index;

        // Escrever packet
        ret = av_interleaved_write_frame(enc->fmt_ctx, enc->pkt);
        if (ret < 0) {
            fprintf(stderr, "Erro ao escrever frame: %s\n", av_err2str(ret));
            return -1;
        }

        av_packet_unref(enc->pkt);
    }

    return 0;
}

// Finalizar encoder
int finalize_encoder(struct encoder *enc) {
    int ret;

    // Enviar NULL para sinalizar fim
    ret = avcodec_send_frame(enc->codec_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Erro ao finalizar encoder: %s\n", av_err2str(ret));
        return -1;
    }

    // Drenar packets restantes
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc->codec_ctx, enc->pkt);
        if (ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Erro ao drenar packets: %s\n", av_err2str(ret));
            return -1;
        }

        av_packet_rescale_ts(enc->pkt, enc->codec_ctx->time_base, enc->video_stream->time_base);
        enc->pkt->stream_index = enc->video_stream->index;

        ret = av_interleaved_write_frame(enc->fmt_ctx, enc->pkt);
        if (ret < 0) {
            fprintf(stderr, "Erro ao escrever packet final: %s\n", av_err2str(ret));
            return -1;
        }

        av_packet_unref(enc->pkt);
    }

    // Escrever trailer
    av_write_trailer(enc->fmt_ctx);

    printf("Gravação finalizada: %ld frames escritos\n", enc->frame_count);
    return 0;
}

// Limpar encoder
void cleanup_encoder(struct encoder *enc) {
    if (enc->pkt) av_packet_free(&enc->pkt);
    if (enc->frame) av_frame_free(&enc->frame);
    if (enc->codec_ctx) avcodec_free_context(&enc->codec_ctx);
    if (enc->fmt_ctx) {
        if (!(enc->fmt_ctx->oformat->flags & AVFMT_NOFILE))
            avio_closep(&enc->fmt_ctx->pb);
        avformat_free_context(enc->fmt_ctx);
    }
    if (enc->sws_ctx) sws_freeContext(enc->sws_ctx);
}

// Reutilizar funções da webcam do código anterior
int init_device(struct camera *cam, const char *device_name) {
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;

    cam->fd = open(device_name, O_RDWR | O_NONBLOCK);
    if (cam->fd == -1) {
        fprintf(stderr, "Erro ao abrir %s: %s\n", device_name, strerror(errno));
        return -1;
    }

    if (ioctl(cam->fd, VIDIOC_QUERYCAP, &cap) == -1) {
        fprintf(stderr, "Erro ao consultar capacidades: %s\n", strerror(errno));
        return -1;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) || !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Dispositivo não suporta captura com streaming\n");
        return -1;
    }

    printf("Webcam: %s (%s)\n", cap.card, cap.driver);

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = 640;
    fmt.fmt.pix.height = 480;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) == -1) {
        fprintf(stderr, "Erro ao configurar formato: %s\n", strerror(errno));
        return -1;
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;
    cam->format = fmt.fmt.pix.pixelformat;

    printf("Resolução: %dx%d\n", cam->width, cam->height);

    memset(&req, 0, sizeof(req));
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) == -1) {
        fprintf(stderr, "Erro ao solicitar buffers: %s\n", strerror(errno));
        return -1;
    }

    cam->buffers = calloc(req.count, sizeof(struct buffer));
    if (!cam->buffers) {
        fprintf(stderr, "Erro ao alocar memória para buffers\n");
        return -1;
    }

    cam->n_buffers = req.count;

    for (unsigned int i = 0; i < cam->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            fprintf(stderr, "Erro ao consultar buffer %d: %s\n", i, strerror(errno));
            return -1;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                    MAP_SHARED, cam->fd, buf.m.offset);

        if (cam->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "Erro ao mapear buffer %d: %s\n", i, strerror(errno));
            return -1;
        }
    }

    return 0;
}

int start_capture(struct camera *cam) {
    for (unsigned int i = 0; i < cam->n_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) == -1) {
            fprintf(stderr, "Erro ao enfileirar buffer %d: %s\n", i, strerror(errno));
            return -1;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) == -1) {
        fprintf(stderr, "Erro ao iniciar streaming: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int capture_and_encode_frame(struct camera *cam, struct encoder *enc) {
    struct v4l2_buffer buf;
    fd_set fds;
    struct timeval tv;

    FD_ZERO(&fds);
    FD_SET(cam->fd, &fds);

    tv.tv_sec = 1;
    tv.tv_usec = 0;

    int r = select(cam->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) {
        fprintf(stderr, "Erro em select(): %s\n", strerror(errno));
        return -1;
    }

    if (r == 0) {
        return 0; // Timeout, continuar
    }

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) == -1) {
        fprintf(stderr, "Erro ao desenfileirar buffer: %s\n", strerror(errno));
        return -1;
    }

    // Codificar frame
    if (encode_frame(enc, (unsigned char*)cam->buffers[buf.index].start) < 0) {
        fprintf(stderr, "Erro ao codificar frame\n");
        return -1;
    }

    if (ioctl(cam->fd, VIDIOC_QBUF, &buf) == -1) {
        fprintf(stderr, "Erro ao reenfileirar buffer: %s\n", strerror(errno));
        return -1;
    }

    return 1; // Frame capturado com sucesso
}

void stop_capture(struct camera *cam) {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);
}

void cleanup_camera(struct camera *cam) {
    if (cam->buffers) {
        for (unsigned int i = 0; i < cam->n_buffers; i++) {
            if (cam->buffers[i].start != MAP_FAILED) {
                munmap(cam->buffers[i].start, cam->buffers[i].length);
            }
        }
        free(cam->buffers);
    }

    if (cam->fd != -1) {
        close(cam->fd);
    }
}

int main(int argc, char *argv[]) {
    const char *device_name = "/dev/video0";
    const char *output_file = "webcam_video.mp4";
    struct camera cam = {0};
    struct encoder enc = {0};

    if (argc > 1) device_name = argv[1];
    if (argc > 2) output_file = argv[2];

    // Configurar handler para interrupção
    signal(SIGINT, signal_handler);

    printf("=== Gravador de Vídeo Webcam ===\n");
    printf("Dispositivo: %s\n", device_name);
    printf("Arquivo de saída: %s\n", output_file);
    printf("Duração: %d segundos\n", RECORDING_DURATION);
    printf("FPS: %d\n\n", TARGET_FPS);

    // Inicializar câmera
    if (init_device(&cam, device_name) != 0) {
        fprintf(stderr, "Erro na inicialização da câmera\n");
        return 1;
    }

    // Inicializar encoder
    if (init_encoder(&enc, output_file, cam.width, cam.height, TARGET_FPS) != 0) {
        fprintf(stderr, "Erro na inicialização do encoder\n");
        cleanup_camera(&cam);
        return 1;
    }

    // Iniciar captura
    if (start_capture(&cam) != 0) {
        fprintf(stderr, "Erro ao iniciar captura\n");
        cleanup_encoder(&enc);
        cleanup_camera(&cam);
        return 1;
    }

    printf("Iniciando gravação... (Ctrl+C para parar)\n");

    time_t start_time = time(NULL);
    int total_frames = 0;
    int target_frames = RECORDING_DURATION * TARGET_FPS;

    // Loop principal de gravação
    while (keep_recording && total_frames < target_frames) {
        int result = capture_and_encode_frame(&cam, &enc);
        if (result > 0) {
            total_frames++;

            // Mostrar progresso a cada segundo
            if (total_frames % TARGET_FPS == 0) {
                int seconds_recorded = total_frames / TARGET_FPS;
                printf("Gravado: %d/%d segundos (%d frames)\n",
                       seconds_recorded, RECORDING_DURATION, total_frames);
            }
        } else if (result < 0) {
            break;
        }

        // Controle de taxa de frames (aproximado)
        usleep(1000000 / TARGET_FPS);
    }

    printf("\nFinalizando gravação...\n");

    // Parar captura e finalizar encoder
    stop_capture(&cam);
    finalize_encoder(&enc);

    // Limpeza
    cleanup_encoder(&enc);
    cleanup_camera(&cam);

    time_t end_time = time(NULL);
    double duration = difftime(end_time, start_time);

    printf("\n=== Gravação Concluída ===\n");
    printf("Arquivo: %s\n", output_file);
    printf("Frames gravados: %d\n", total_frames);
    printf("Duração real: %.1f segundos\n", duration);
    printf("FPS médio: %.1f\n", total_frames / duration);

    return 0;
}
