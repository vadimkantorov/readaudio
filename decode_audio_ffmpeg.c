// based on https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/decode_audio.c and https://github.com/FFmpeg/FFmpeg/blob/master/doc/examples/demuxing_decoding.c

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>

// https://github.com/dmlc/dlpack/blob/master/include/dlpack/dlpack.h
#include "dlpack.h"

void deleter(struct DLManagedTensor* self)
{
	fprintf(stderr, "Deleter calling\n");
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

	fprintf(stderr, "Deleter called\n");
}

void __attribute__ ((constructor)) onload()
{
	//needed before ffmpeg 4.0, deprecated in ffmpeg 4.0
	av_register_all();
	fprintf(stderr, "Ffmpeg initialized\n");
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

int decode_packet(AVCodecContext *avctx, AVPacket *pkt, uint8_t** data, int itemsize)
{
	AVFrame *frame = av_frame_alloc();
	int ret = avcodec_send_packet(avctx, pkt);
	
	while (ret >= 0)
	{
		ret = avcodec_receive_frame(avctx, frame);
		if (ret == 0)
		{
			//data = memcpy(*data, frame->data) + itemsize * frame->nb_samples * avctx->channels;
			
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

struct DecodeAudio decode_audio(const char* input_path)
{
	struct DecodeAudio audio = {0};

	AVFormatContext *pFormatCtx = NULL;
	AVCodecContext* pCodecCtx = NULL;
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

	pCodecCtx = avcodec_alloc_context3(codec);
	if (!pCodecCtx)
	{
		strcpy(audio.error, "Could not allocate audio codec context");
		goto end;
	}

	if (avcodec_parameters_to_context(pCodecCtx, stream->codecpar) < 0)
	{
		strcpy(audio.error, "Failed to copy audio codec parameters to decoder context");
		goto end;
	}

	if (avcodec_open2(pCodecCtx, codec, NULL) < 0)
	{
		strcpy(audio.error, "Could not open codec");
		goto end;
	}

	enum AVSampleFormat sample_fmt = pCodecCtx->sample_fmt;
	if (av_sample_fmt_is_planar(sample_fmt))
	{
		const char *packed = av_get_sample_fmt_name(sample_fmt);
		printf("Warning: the sample format the decoder produced is planar (%s). This example will output the first channel only.\n", packed ? packed : "?");
		sample_fmt = av_get_packed_sample_fmt(pCodecCtx->sample_fmt);
	}
    
	static struct sample_fmt_entry {enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le; DLDataType dtype;} sample_fmt_entries[] =
	{
		{ AV_SAMPLE_FMT_U8,  "u8"   ,    "u8" , { kDLUInt  , 8 , 1 }},
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" , { kDLInt   , 16, 1 }},
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" , { kDLInt   , 32, 1 }},
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" , { kDLFloat , 32, 1 }},
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" , { kDLFloat , 64, 1 }},
	};

	int i;
	DLDataType dtype;
	for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++)
	{
		struct sample_fmt_entry *entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt)
		{
            strcpy(audio.fmt, AV_NE(entry->fmt_be, entry->fmt_le));
			dtype = entry->dtype;
			i = -1;
			break;
		}
	}

	if (i != -1)
	{
		strcpy(audio.error, "Could not deduce format");
		goto end;
	}

	audio.sample_rate = pCodecCtx->sample_rate;
	audio.num_channels = pCodecCtx->channels;
	audio.num_samples  = (pFormatCtx->duration / (float) AV_TIME_BASE) * audio.sample_rate;
	audio.data.deleter = deleter;
	audio.data.dl_tensor.ctx.device_type = kDLCPU;
	audio.data.dl_tensor.ndim = 2;
	audio.data.dl_tensor.dtype = dtype; 
	audio.data.dl_tensor.shape = malloc(audio.data.dl_tensor.ndim * sizeof(int64_t));
	audio.data.dl_tensor.shape[0] = audio.num_samples;
	audio.data.dl_tensor.shape[1] = audio.num_channels;
	audio.data.dl_tensor.strides = malloc(audio.data.dl_tensor.ndim * sizeof(int64_t));
	audio.data.dl_tensor.strides[0] = audio.num_channels;
	audio.data.dl_tensor.strides[1] = 1;
	audio.itemsize = audio.data.dl_tensor.dtype.lanes * audio.data.dl_tensor.dtype.bits / 8;
	audio.data.dl_tensor.data = calloc(audio.num_samples * audio.num_channels, audio.itemsize);
	
	uint8_t* data_ptr = audio.data.dl_tensor.data;
	pkt = av_packet_alloc();
	while (av_read_frame(pFormatCtx, pkt) >= 0)
	{
		if (pkt->stream_index == stream_index && decode_packet(pCodecCtx, pkt, &data_ptr, audio.itemsize) < 0)
			break;
		av_packet_unref(pkt);
	}

	pkt->data = NULL;
	pkt->size = 0;
	decode_packet(pCodecCtx, pkt, &data_ptr, audio.itemsize);

end:
	if(pCodecCtx)
		avcodec_free_context(&pCodecCtx);
	if(pkt)
		av_packet_free(&pkt);
	return audio;
}

int main(int argc, char **argv)
{
	if (argc <= 2)
	{
		printf("Usage: %s <input file> <output file>\n", argv[0]);
		return 1;
	}
	
	struct DecodeAudio audio = decode_audio(argv[1]);
	
	printf("ffplay -f %s -ac %d -ar %d -i %s # num samples: %d\n", audio.fmt, (int)audio.num_channels, (int)audio.sample_rate, argv[1], (int)audio.num_samples);
	fwrite(audio.data.dl_tensor.data, audio.itemsize, audio.num_samples * audio.num_channels, fopen(argv[2], "wb"));
	return 0;
}
