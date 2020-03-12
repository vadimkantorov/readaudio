```shell
# create sample audio test.wav
ffmpeg -f lavfi -i "sine=frequency=1000:duration=5" -c:a pcm_s16le -ar 8000 test.wav

gcc -o decode_audio decode_audio.c -lavformat -lavcodec -lswresample -lavutil

./decode_audio test.wav out.bin

ffplay -f s16le -ac 1 -ar 8000 out.bin
```
