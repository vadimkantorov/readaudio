This repo is a primer in reading audio (via ffmpeg) into NumPy/PyTorch arrays without copying data or process launching. Interfacing with FFmpeg is done in pure C code in [decode_audio.c](./decode_audio.c). Python wrapper is implemented in [decode_audio.py](./decode_audio.py) using a standard library module ctypes. C code returns a plain structure [Audio](./decode_audio.c#L12-L20). This structure is then interpeted and wrapped by NumPy or PyTorch without copy.

At the bottom there is example of alternative solution using process launching. The first solution is preferable if you must load huge amounts of audio in various formats (for reading `*.wav` files, there is a standard Python [`wave`](https://docs.python.org/3/library/wave.html) module and [`scipy.io.wavfile.read`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.io.wavfile.read.html).

It is also a simple primer on FFmpeg audio decoding loop and basic ctypes usage for interfacing C code and NumPy/PyTorch (without creating a full-blown PyTorch C++ extension).

```shell
# create sample audio test.wav
ffmpeg -f lavfi -i "sine=frequency=1000:duration=5" -c:a pcm_s16le -ar 8000 test.wav

# convert audio to raw format
ffmpeg -i test.wav -f s16le -acodec pcm_s16le golden.raw

# play a raw file
ffplay -f s16le -ac 1 -ar 8000 golden.raw

# compile executable for testing
gcc -o decode_audio decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format and compare to golden
./decode_audio test.wav bin.raw
diff golden.raw bin.raw

# compile a shared library for interfacing with NumPy
gcc -o decode_audio.so -shared -fPIC decode_audio.c -lavformat -lavcodec -lswresample -lavutil

# convert audio to raw format (NumPy) and compare to golden
python3 decode_audio.py test.wav numpy.raw
diff golden.raw numpy.raw

# convert audio to raw format (PyTorch) and compare to golden
python3 decode_audio.py test.wav torch.raw
diff golden.raw torch.raw
```

```python
# read_audio.py. read audio using subprocess
# python3 read_audio.py test.wav

import sys
import subprocess
import struct

format_ffmpeg, format_struct = [('s16le', 'h'), ('f32le', 'f'), ('u8', 'B'), ('s8', 'b')][0]
sample_rate = 8_000 # resample
num_channels = 1 # force mono

audio = memoryview(subprocess.check_output(['ffmpeg', '-nostdin', '-hide_banner', '-nostats', '-loglevel', 'quiet', '-i', sys.argv[1], '-f', format_ffmpeg, '-ar', str(sample_rate), '-ac', str(num_channels), '-']))
audio = audio.cast(format_struct, shape = [len(audio) // num_channels // struct.calcsize(format_struct), num_channels])

print('shape', audio.shape, 'itemsize', audio.itemsize, 'format', audio.format)
# shape (40000, 1) itemsize 2 format h
```
