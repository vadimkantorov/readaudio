// based on https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_audio.c and https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/demuxing_decoding.c and https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/filtering_audio.c


#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

// https://github.com/dmlc/dlpack/blob/master/include/dlpack/dlpack.h
#include "dlpack.h"

void deleter(struct DLManagedTensor* self)
{
	if(self->dl_tensor.data)
	{
		free(self->dl_tensor.data);
		self->dl_tensor.data = NULL;
	}

	if(self->dl_tensor.shape)
	{
		free(self->dl_tensor.shape);
		self->dl_tensor.shape = NULL;
	}
	
	if(self->dl_tensor.strides)
	{
		free(self->dl_tensor.strides);
		self->dl_tensor.strides = NULL;
	}
}

void __attribute__ ((constructor)) onload()
{
	//needed before ffmpeg 4.0, deprecated in ffmpeg 4.0
	av_register_all();
	avfilter_register_all();
}

struct DecodeAudio
{
	char error[128];
	char fmt[8];
	uint64_t sample_rate;
	uint64_t num_channels;
	uint64_t num_samples;
	uint64_t itemsize;
	DLManagedTensor data;
};

void process_output_frame(uint8_t** data, AVFrame* frame, int num_samples, int num_channels, uint64_t* data_len, int itemsize)
{
	//data = memcpy(*data, frame->data) + itemsize * frame->nb_samples * av_ctx->channels;
	
	for (int i = 0; i < num_samples; i++)
	{
		for (int c = 0; c < num_channels; c++)
		{
			if(*data_len >= itemsize)
			{
				*data = memcpy(*data, frame->data[c] + itemsize * i, itemsize) + itemsize;
				*data_len -= itemsize;
			}
		}
	}
}

int decode_packet(AVCodecContext *av_ctx, AVFilterContext* buffersrc_ctx, AVFilterContext* buffersink_ctx, AVPacket *pkt, uint8_t** data, uint64_t* data_len, int itemsize)
{
	AVFrame *frame = av_frame_alloc();
	AVFrame *filt_frame = av_frame_alloc();

	int ret = avcodec_send_packet(av_ctx, pkt);

	int filtering = buffersrc_ctx != NULL && buffersink_ctx != NULL;
	while (ret >= 0)
	{
		ret = avcodec_receive_frame(av_ctx, frame);
		if (ret == 0)
		{
			if(filtering)
			{
				ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
				if(ret < 0)
					goto end;
			}
			
			while (filtering)
			{
				ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					break;
				if (ret < 0)
					goto end;
				process_output_frame(data, filt_frame, filt_frame->nb_samples, av_ctx->channels, data_len, itemsize);
				av_frame_unref(filt_frame);
			}
			
			if(!filtering)
				process_output_frame(data, frame, frame->nb_samples, av_ctx->channels, data_len, itemsize);
			//av_frame_unref(frame);
		}
	}

end:
	if (ret == AVERROR(EAGAIN))
		ret = 0;
	
	av_frame_free(&frame);
	av_frame_free(&filt_frame);
	return ret;
}

struct avio_cursor
{
	uint8_t *base;
	size_t size;
    uint8_t *ptr;
    size_t left;
};

static int buffer_read(void *opaque, uint8_t *buf, int buf_size)
{
    struct avio_cursor *cursor = (struct avio_cursor *)opaque;
    buf_size = FFMIN(buf_size, cursor->left);

    if (!buf_size)
        return AVERROR_EOF;

    memcpy(buf, cursor->ptr, buf_size);
    cursor->ptr  += buf_size;
    cursor->left -= buf_size;
    return buf_size;
}

static int64_t buffer_seek(void* opaque, int64_t offset, int whence)
{
    struct avio_cursor *cursor = (struct avio_cursor *)opaque;
	if(whence == AVSEEK_SIZE)
		return cursor->size;

	cursor->ptr = cursor->base + offset;
	cursor->left = cursor->size - offset;
	return offset;
}

size_t nbytes(struct DecodeAudio* audio)
{
	size_t itemsize = audio->data.dl_tensor.dtype.lanes * audio->data.dl_tensor.dtype.bits / 8;
	size_t size = 1;
	for(size_t i = 0; i < audio->data.dl_tensor.ndim; i++)
		size *= audio->data.dl_tensor.shape[i];
	return size * itemsize;
}

struct DecodeAudio decode_audio(const char* input_path, int probe, struct DecodeAudio input_options, struct DecodeAudio output_options, const char* filter_string)
{
	struct DecodeAudio audio = { 0 };

	AVIOContext* io_ctx = NULL;
	AVFormatContext* fmt_ctx = NULL;
	AVCodecContext* dec_ctx = NULL;
	AVPacket* pkt = NULL;
	char buffersrc_args[512];
	AVFilterGraph *graph = NULL;
	AVFilterInOut *gis = avfilter_inout_alloc();
    AVFilterInOut *gos = avfilter_inout_alloc();
	AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
	AVFilter *buffersrc  = avfilter_get_by_name("abuffer");
    AVFilter *buffersink = avfilter_get_by_name("abuffersink");
	assert(buffersrc != NULL && buffersink != NULL);
	uint8_t* avio_ctx_buffer = NULL;
    struct avio_cursor cursor = { 0 };

	if(input_path == NULL)
	{
		int avio_ctx_buffer_size = 4096;
    	avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
		assert(avio_ctx_buffer);
    	
		cursor.base = cursor.ptr  = (uint8_t*)input_options.data.dl_tensor.data;
    	cursor.size = cursor.left = nbytes(&input_options);
		io_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size, 0, &cursor, &buffer_read, NULL, &buffer_seek);
		if(!io_ctx)
		{
			strcpy(audio.error, "Cannot allocate IO context");
			goto end;
		}

		fmt_ctx = avformat_alloc_context();
		fmt_ctx->pb = io_ctx;
	}

	if (avformat_open_input(&fmt_ctx, input_path, NULL, NULL) != 0)
	{
		strcpy(audio.error, "Cannot open file");
		goto end;
	}

	if (avformat_find_stream_info(fmt_ctx, NULL) < 0)
	{
		strcpy(audio.error, "Cannot open find stream information");
		goto end;
	}

	int stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream_index < 0)
	{
		strcpy(audio.error, "Cannot find audio stream");
		goto end;
	}
	AVStream *stream = fmt_ctx->streams[stream_index];
	
	AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec)
	{
		strcpy(audio.error, "Codec not found");
		goto end;
	}

	dec_ctx = avcodec_alloc_context3(codec);
	if (!dec_ctx)
	{
		strcpy(audio.error, "Cannot allocate audio codec context");
		goto end;
	}

	if (avcodec_parameters_to_context(dec_ctx, stream->codecpar) < 0)
	{
		strcpy(audio.error, "Failed to copy audio codec parameters to decoder context");
		goto end;
	}

	if (avcodec_open2(dec_ctx, codec, NULL) < 0)
	{
		strcpy(audio.error, "Cannot open codec");
		goto end;
	}

	enum AVSampleFormat sample_fmt = dec_ctx->sample_fmt;
	if (av_sample_fmt_is_planar(sample_fmt))
	{
		const char *packed = av_get_sample_fmt_name(sample_fmt);
		printf("Warning: the sample format the decoder produced is planar (%s). This example will output the first channel only.\n", packed ? packed : "?");
		sample_fmt = av_get_packed_sample_fmt(dec_ctx->sample_fmt);
	}
	static struct sample_fmt_entry {enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le; DLDataType dtype;} supported_sample_fmt_entries[] =
	{
		{ AV_SAMPLE_FMT_U8,  "u8"   ,    "u8" , { kDLUInt  , 8 , 1 }},
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" , { kDLInt   , 16, 1 }},
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" , { kDLInt   , 32, 1 }},
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" , { kDLFloat , 32, 1 }},
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" , { kDLFloat , 64, 1 }},
	};
	
	double in_duration = fmt_ctx->duration / (float) AV_TIME_BASE; assert(in_duration > 0);
	int in_sample_rate = dec_ctx->sample_rate;
	int in_num_channels = dec_ctx->channels;
	uint64_t in_num_samples  = in_duration * audio.sample_rate;
	
	DLDataType in_dtype, out_dtype;
	enum AVSampleFormat in_sample_fmt = AV_SAMPLE_FMT_NONE, out_sample_fmt = AV_SAMPLE_FMT_NONE; 
	for (int k = 0; k < FF_ARRAY_ELEMS(supported_sample_fmt_entries); k++)
	{
		struct sample_fmt_entry* entry = &supported_sample_fmt_entries[k];
		
		if (sample_fmt == entry->sample_fmt)
		{
			in_dtype = entry->dtype;
			in_sample_fmt = entry->sample_fmt;
            strcpy(audio.fmt, AV_NE(entry->fmt_be, entry->fmt_le));
		}

		if (strcmp(output_options.fmt, entry->fmt_le) == 0 || strcmp(output_options.fmt, entry->fmt_be) == 0)
		{
			out_dtype = entry->dtype;
			out_sample_fmt = entry->sample_fmt;
		}
	}
	if (in_sample_fmt == AV_SAMPLE_FMT_NONE)
	{
		strcpy(audio.error, "Cannot deduce format");
		goto end;
	}
	if (out_sample_fmt == AV_SAMPLE_FMT_NONE)
	{
		out_sample_fmt = in_sample_fmt;
		out_dtype = in_dtype;
	}
	int out_sample_rate = output_options.sample_rate > 0 ? output_options.sample_rate : in_sample_rate;

	if (!dec_ctx->channel_layout)
		dec_ctx->channel_layout = av_get_default_channel_layout(dec_ctx->channels);
	uint64_t channel_layout = dec_ctx->channel_layout;

	//double in_duration = stream->time_base.num * (int)stream->duration / stream->time_base.den;
	
	audio.sample_rate = out_sample_rate;
	audio.num_channels = dec_ctx->channels;
	audio.num_samples = in_duration * out_sample_rate;
	audio.data.dl_tensor.ctx.device_type = kDLCPU;
	audio.data.dl_tensor.ndim = 2;
	audio.data.dl_tensor.dtype = out_dtype; 
	audio.data.dl_tensor.shape = malloc(audio.data.dl_tensor.ndim * sizeof(int64_t));
	audio.data.dl_tensor.shape[0] = audio.num_samples;
	audio.data.dl_tensor.shape[1] = audio.num_channels;
	audio.data.dl_tensor.strides = malloc(audio.data.dl_tensor.ndim * sizeof(int64_t));
	audio.data.dl_tensor.strides[0] = audio.data.dl_tensor.shape[1];
	audio.data.dl_tensor.strides[1] = 1;
	audio.itemsize = audio.data.dl_tensor.dtype.lanes * audio.data.dl_tensor.dtype.bits / 8;
	
	if(probe)
		goto end;
    
	if(out_sample_rate != in_sample_rate)
	{
		filter_string = "aresample=sample_rate=16000";
	}

	if(filter_string != NULL)
	{
		graph = avfilter_graph_alloc();
		if(!graph)
		{
			strcpy(audio.error, "Cannot allocate filter graph");
			goto end;
		}

		snprintf(buffersrc_args, sizeof(buffersrc_args), "sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64":time_base=%d/%d", in_sample_rate, av_get_sample_fmt_name(in_sample_fmt), channel_layout, dec_ctx->time_base.num, dec_ctx->time_base.den);

		if (avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", buffersrc_args, NULL, graph) < 0)
		{
			strcpy(audio.error, "Cannot create buffer source");
			goto end;
		}

		int ret;
		if ((ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, graph)) < 0)
		{
			fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
			strcpy(audio.error, "Cannot create buffer sink");
			goto end;
		}

		const enum AVSampleFormat out_sample_fmts[] = { out_sample_fmt, -1 };
		if (av_opt_set_int_list(buffersink_ctx, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN) < 0)
		{
			strcpy(audio.error, "Cannot set output sample format");
			goto end;
		}
		const int64_t out_channel_layouts[] = { channel_layout , -1 };
		if (av_opt_set_int_list(buffersink_ctx, "channel_layouts", out_channel_layouts, -1, AV_OPT_SEARCH_CHILDREN) < 0) 
		{
			strcpy(audio.error, "Cannot set output channel layout");
			goto end;
		}
		const int out_sample_rates[] = { out_sample_rate, -1 };
		if (av_opt_set_int_list(buffersink_ctx, "sample_rates", out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN) < 0)
		{
			strcpy(audio.error, "Cannot set output sample rate");
			goto end;
		}

		gis->name = av_strdup("out");
		gis->filter_ctx = buffersink_ctx;
		gis->pad_idx = 0;
		gis->next = NULL;
		
		gos->name = av_strdup("in");
		gos->filter_ctx = buffersrc_ctx;
		gos->pad_idx = 0;
		gos->next = NULL;
		
		if(avfilter_graph_parse_ptr(graph, filter_string, &gis, &gos, NULL) < 0)
		{
			strcpy(audio.error, "Cannot parse graph");
			goto end;
		}

		if(avfilter_graph_config(graph, NULL) < 0)
		{
			strcpy(audio.error, "Cannot configure graph.");
			goto end;
		}
	}

	uint64_t data_len = 0;
	if(output_options.data.dl_tensor.data)
	{
		data_len = nbytes(&output_options);
		audio.data.dl_tensor.data = output_options.data.dl_tensor.data;
	}
	else
	{
		audio.data.deleter = deleter;
		data_len = audio.num_samples * audio.num_channels * audio.itemsize;
		audio.data.dl_tensor.data = calloc(data_len, 1);
	}

	uint8_t* data_ptr = audio.data.dl_tensor.data;

	pkt = av_packet_alloc();
	while (av_read_frame(fmt_ctx, pkt) >= 0)
	{
		if (pkt->stream_index == stream_index && decode_packet(dec_ctx, buffersrc_ctx, buffersink_ctx, pkt, &data_ptr, &data_len, audio.itemsize) < 0)
			break;
		av_packet_unref(pkt);
	}

	pkt->data = NULL;
	pkt->size = 0;
	decode_packet(dec_ctx, buffersrc_ctx, buffersink_ctx, pkt, &data_ptr, &data_len, audio.itemsize);

end:
	if(graph)
		avfilter_graph_free(&graph);
	if(dec_ctx)
		avcodec_free_context(&dec_ctx);
	if(fmt_ctx)
		avformat_close_input(&fmt_ctx);
	if(pkt)
		av_packet_free(&pkt);
	if(gis)
		avfilter_inout_free(&gis);
	if(gos)
		avfilter_inout_free(&gos);
    if(io_ctx)
		av_free(io_ctx);
	
	//fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
	return audio;
}

int main(int argc, char **argv)
{
	if (argc <= 2)
	{
		printf("Usage: %s <input file> <output file> <filter string>\n", argv[0]);
		return 1;
	}
	
	//struct DecodeAudio audio = decode_audio(argv[1], NULL, 0, argc == 4 ? argv[3] : NULL);
	char buf[100000];
	int64_t read = fread(buf, 1, sizeof(buf), fopen(argv[1], "r"));
	struct DecodeAudio input_options = { 0 }, output_options = { 0 };
	input_options.data.dl_tensor.data = &buf;
	input_options.data.dl_tensor.ndim = 1;
	input_options.data.dl_tensor.shape = &read;
	input_options.data.dl_tensor.dtype.lanes = 1;
	input_options.data.dl_tensor.dtype.bits = 8;
	input_options.data.dl_tensor.dtype.code = kDLUInt;
	
	struct DecodeAudio audio = decode_audio(NULL, false, input_options, output_options, argc == 4 ? argv[3] : NULL);
	
	printf("Error: [%s]\n", audio.error);
	printf("ffplay -i %s\n", argv[1]);
	printf("ffplay -f %s -ac %d -ar %d -i %s # num samples: %d\n", audio.fmt, (int)audio.num_channels, (int)audio.sample_rate, argv[2], (int)audio.num_samples);
	fwrite(audio.data.dl_tensor.data, audio.itemsize, audio.num_samples * audio.num_channels, fopen(argv[2], "wb"));
	return 0;
}
