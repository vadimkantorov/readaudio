```shell
# create sample audio test.wav
ffmpeg -f lavfi -i "sine=frequency=1000:duration=5" -c:a pcm_s16le -ar 8000 test.wav

# convert audio to raw format
ffmpeg -i test.wav -f s16le -acodec pcm_s16le out0.raw

# compile executable for testing
gcc -o decode_audio decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format and compare to etalon
./decode_audio test.wav out1.raw
diff out0.raw out1.raw

# play a raw file
ffplay -f s16le -ac 1 -ar 8000 out1.raw

# compile a shared library for interfacing with NumPy
gcc -o decode_audio.so -shared -fPIC decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format and compare to etalon. DOES NOT WORK YET
python3 decode_audio_ctypes.py test.wav out2.raw
diff out0.raw outw.raw
```
