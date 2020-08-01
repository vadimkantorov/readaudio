# Work In Progress

This repo is a primer in reading audio (via ffmpeg) into NumPy/PyTorch arrays without copying data or process launching. Interfacing with FFmpeg is done in pure C code in [decode_audio.c](./decode_audio.c). Python wrapper is implemented in [decode_audio.py](./decode_audio.py) using a standard library module ctypes. C code returns a plain C structure [Audio](./decode_audio.c#L12-L20). This structure is then interpeted and wrapped by NumPy or PyTorch without copy. 

At the bottom is an example of alternative solution using process launching. The first solution is preferable if you must load huge amounts of audio in various formats (for reading `*.wav` files, there exists a standard Python [`wave`](https://docs.python.org/3/library/wave.html) module and [`scipy.io.wavfile.read`](https://docs.scipy.org/doc/scipy/reference/generated/scipy.io.wavfile.read.html)).

It is also a simple primer on FFmpeg audio decoding loop and basic ctypes usage for interfacing C code and NumPy/PyTorch (without creating a full-blown PyTorch C++ extension).

```shell
# install dependencies: ffmpeg executables and shared libraries on ubuntu
apt-get install -y ffmpeg libavcodec-dev libavformat-dev libavfilter-dev
```

```shell
# create sample audio test.wav
ffmpeg -f lavfi -i "sine=frequency=1000:duration=5" -c:a pcm_s16le -ar 8000 test.wav

# convert audio to raw format
ffmpeg -i test.wav -f s16le -acodec pcm_s16le golden.raw

# play a raw file
ffplay -f s16le -ac 1 -ar 8000 golden.raw

# compile executable for testing
make decode_audio_ffmpeg

# convert audio to raw format and compare to golden
./decode_audio_ffmpeg test.wav bin.raw
diff golden.raw bin.raw

# compile a shared library for interfacing with NumPy and PyTorch
make decode_audio_ffmpeg.so

# convert audio to raw format (NumPy) and compare to golden
python3 decode_audio.py -i test.wav -o numpy.raw
diff golden.raw numpy.raw

# convert audio to raw format (PyTorch) and compare to golden
python3 decode_audio.py -i test.wav -o torch.raw
diff golden.raw torch.raw

# convert audio to raw format (PyTorch / DLPack) and compare to golden
python3 decode_audio.py -i test.wav -o dlpack.raw
diff golden.raw dlpack.raw
```

```python
# read audio using subprocess
# python3 decode_audio_subprocess.py test.wav

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

### TODO
- SOX backend ( https://github.com/pytorch/audio/blob/master/torchaudio/torch_sox.cpp)
- ffmpeg audio filter graph
- decode from a buffer
- non-allocating version that keeps allocations in Python for simpler memory management
- probe function
