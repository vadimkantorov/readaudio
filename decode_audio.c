#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

struct Audio
{
	char error[128];
	char fmt[8];
	uint64_t sample_rate;
	uint64_t num_channels;
	uint64_t num_samples;
	uint64_t itemsize;
	uint8_t* data;
};

int decode_packet(AVCodecContext *avctx, AVPacket *pkt, uint8_t** data, int itemsize)
{
	AVFrame *frame = av_frame_alloc();
	int ret = avcodec_send_packet(avctx, pkt);
	
	while (ret >= 0) {
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == 0)
		{
			for (int i = 0; i < frame->nb_samples; i++)
				for (int ch = 0; ch < avctx->channels; ch++)
					*data = memcpy(*data, frame->data[ch] + itemsize * i, itemsize) + itemsize;
		}
	}

	if (ret == AVERROR(EAGAIN))
		ret = 0;
	
	av_frame_free(&frame);
	return ret;
}

struct Audio decode_audio(const char* input_path)
{
	struct Audio audio = {0};
	av_register_all();

	AVFormatContext *pFormatCtx = NULL;
	AVPacket* pkt = NULL;
	
	if (avformat_open_input(&pFormatCtx, input_path, NULL, NULL) != 0)
	{
		strcpy(audio.error, "Could not open file");
		goto end;
	}

	if (avformat_find_stream_info(pFormatCtx, NULL) < 0)
	{
		strcpy(audio.error, "Could not open find stream information");
		goto end;
	}

	int stream_index = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream_index < 0)
	{
		strcpy(audio.error, "Could not find audio stream");
		goto end;
	}
	AVStream *stream = pFormatCtx->streams[stream_index];
	
	AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec)
	{
		strcpy(audio.error, "Codec not found");
		goto end;
	}

	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c)
	{
		strcpy(audio.error, "Could not allocate audio codec context");
		goto end;
	}

	if (avcodec_parameters_to_context(c, stream->codecpar) < 0)
	{
		strcpy(audio.error, "Failed to copy audio codec parameters to decoder context");
		goto end;
	}

	if (avcodec_open2(c, codec, NULL) < 0)
	{
		strcpy(audio.error, "Could not open codec");
		goto end;
	}

	enum AVSampleFormat sample_fmt = c->sample_fmt;
	if (av_sample_fmt_is_planar(sample_fmt))
	{
		const char *packed = av_get_sample_fmt_name(sample_fmt);
		printf("Warning: the sample format the decoder produced is planar (%s). This example will output the first channel only.\n", packed ? packed : "?");
		sample_fmt = av_get_packed_sample_fmt(c->sample_fmt);
	}
    
	struct sample_fmt_entry {enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le; } sample_fmt_entries[] = {
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};

	int i;
	for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++)
	{
		struct sample_fmt_entry *entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt)
		{
            		strcpy(audio.fmt, AV_NE(entry->fmt_be, entry->fmt_le));
			i = -1;
			break;
		}
	}

	if (i != -1)
	{
		strcpy(audio.error, "Could not deduce format");
		goto end;
	}

	audio.num_channels = c->channels;
	audio.sample_rate = c->sample_rate;
	audio.num_samples  = (pFormatCtx->duration / (float) AV_TIME_BASE) * audio.sample_rate;
	audio.itemsize = av_get_bytes_per_sample(sample_fmt);
	audio.data = calloc(audio.num_samples * audio.num_channels, audio.itemsize);

	uint8_t* data_ptr = audio.data;
	pkt = av_packet_alloc();
	while (av_read_frame(pFormatCtx, pkt) >= 0)
	{
		if (pkt->stream_index == stream_index && decode_packet(c, pkt, &data_ptr, audio.itemsize) < 0)
			break;
		av_packet_unref(pkt);
	}

	pkt->data = NULL;
	pkt->size = 0;
	decode_packet(c, pkt, &data_ptr, audio.itemsize);

end:
	if(c)
		avcodec_free_context(&c);
	if(pkt)
		av_packet_free(&pkt);
	return audio;
}

int main(int argc, char **argv)
{
    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(0);
    }
	
	struct Audio audio = decode_audio(argv[1]);
    
	printf("ffplay -f %s -ac %d -ar %d -i %s # num samples: %d\n", audio.fmt, (int)audio.num_channels, (int)audio.sample_rate, argv[1], (int)audio.num_samples);
	FILE *out = fopen(argv[2], "wb");
	fwrite(audio.data, audio.itemsize, audio.num_samples * audio.num_channels, out);
	fclose(out);
}
