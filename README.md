```shell
# create sample audio test.wav
ffmpeg -f lavfi -i "sine=frequency=1000:duration=5" -c:a pcm_s16le -ar 8000 test.wav

# compile executable for testing
gcc -o decode_audio decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format
./decode_audio test.wav out1.bin

# play a raw file
ffplay -f s16le -ac 1 -ar 8000 out1.bin

# compile a shared library for interfacing with NumPy
gcc -o decode_audio.so -shared decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# (DOES NOT WORK YET) convert audio to raw format
python3 decode_audio_ctypes.py test.wav out2.bin
```
