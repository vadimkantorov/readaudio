```shell
# create sample audio test.wav
ffmpeg -f lavfi -i "sine=frequency=1000:duration=5" -c:a pcm_s16le -ar 8000 test.wav

# convert audio to raw format
ffmpeg -i test.wav -f s16le -acodec pcm_s16le golden.raw

# play a raw file
ffplay -f s16le -ac 1 -ar 8000 golden.raw

# compile executable for testing
gcc -o decode_audio decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format and compare to etalon
./decode_audio test.wav bin.raw
diff golden.raw bin.raw

# compile a shared library for interfacing with NumPy
gcc -o decode_audio.so -shared -fPIC decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format (NumPy) and compare to etalon
python3 decode_audio.py test.wav numpy.raw
diff golden.raw numpy.raw

# convert audio to raw format (PyTorch) and compare to etalon
python3 decode_audio.py test.wav torch.raw
diff golden.raw torch.raw
```
