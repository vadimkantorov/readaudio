CC = gcc

LIBS_FFMPEG = -lavformat -lavcodec -lavfilter -lavutil

SHAREDFLAGS = -shared -fPIC

#CFLAGS = -g
CFLAGS = -O3

ffmpeg: avio_reading decode_audio_ffmpeg decode_audio_ffmpeg.so

avio_reading: avio_reading.c
	$(CC) -o $@ $< $(LIBS_FFMPEG) $(CFLAGS)
	

decode_audio_ffmpeg: decode_audio_ffmpeg.c
	$(CC) -o $@ $< $(LIBS_FFMPEG) $(CFLAGS)

decode_audio_ffmpeg.so: decode_audio_ffmpeg.c
	$(CC) -o $@ $(SHAREDFLAGS) $< $(LIBS_FFMPEG) $(CFLAGS)

clean:
	rm -f decode_audio_ffmpeg decode_audio_ffmpeg.so

.PHONY: clean ffmpeg
