#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr, "sample format %s is not supported as output format\n", av_get_sample_fmt_name(sample_fmt));
    return -1;
}

int decode_packet(AVCodecContext *avctx, AVPacket *pkt, FILE* outfile)
{
	AVFrame *frame = av_frame_alloc();
	int ret = avcodec_send_packet(avctx, pkt);
	
	while (ret >= 0) {
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == 0)
		{
			int data_size = av_get_bytes_per_sample(frame->format);
			for (int i = 0; i < frame->nb_samples; i++)
				for (int ch = 0; ch < avctx->channels; ch++)
					fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
		}
	}

	if (ret == AVERROR(EAGAIN))
		ret = 0;
	
	av_frame_free(&frame);
	return ret;
}

struct Audio
{
	char fmt[8];
	uint8_t num_channels;
	uint64_t num_samples;
	void* data;
};

struct Audio decode_audio_(const char* input_path)
{
	puts(input_path);
	return (struct Audio) {.fmt = "ABCDEFG", .num_channels = 2, .num_samples = 100, .data = NULL};
}

int main(int argc, char **argv)
{
    AVCodec *codec = NULL;
    AVCodecContext *c = NULL;
    AVCodecParserContext *parser = NULL;

    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(0);
    }
    const char* filename    = argv[1];
    const char* outfilename = argv[2];

	av_register_all();
	AVFormatContext *pFormatCtx;// = avformat_alloc_context();

	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
        fprintf(stderr, "Could not open file\n");
        exit(1);
    }

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        fprintf(stderr, "Could not open find stream information\n");
        exit(1);
	}

	int stream_index = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream_index < 0) {
		fprintf(stderr, "Could not find %s stream in input file '%s'\n", av_get_media_type_string(AVMEDIA_TYPE_AUDIO), filename);
		exit(1);
	}
	AVStream *stream = pFormatCtx->streams[stream_index];
	
	codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

	if (avcodec_parameters_to_context(c, stream->codecpar) < 0) {
	    fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n", av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
		exit(1);
	}

    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    FILE *outfile = fopen(outfilename, "wb");

    AVPacket* pkt = av_packet_alloc();
	while (av_read_frame(pFormatCtx, pkt) >= 0)
	{
		if (pkt->stream_index == stream_index && decode_packet(c, pkt, outfile) < 0)
			break;
		av_packet_unref(pkt);
	}

    pkt->data = NULL;
    pkt->size = 0;
	decode_packet(c, pkt, outfile);

    enum AVSampleFormat sfmt = c->sample_fmt;
	if (av_sample_fmt_is_planar(sfmt)) {
        const char *packed = av_get_sample_fmt_name(sfmt);
        printf("Warning: the sample format the decoder produced is planar "
               "(%s). This example will output the first channel only.\n",
               packed ? packed : "?");
        sfmt = av_get_packed_sample_fmt(c->sample_fmt);
    }

    const char *fmt = NULL;
    if (get_format_from_sample_fmt(&fmt, sfmt) < 0)
        goto end;

    printf("Play the output audio file with the command:\nffplay -f %s -ac %d -ar %d %s\n", fmt, c->channels, c->sample_rate, outfilename);
end:
    fclose(outfile);
    avcodec_free_context(&c);
    av_packet_free(&pkt);

    return 0;
}
