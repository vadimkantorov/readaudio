import sys
import ctypes
import numpy as np

class DecodeAudio(ctypes.Structure):
	_fields_ = [
		('fmt', ctypes.c_char * 8),
		('num_channels', ctypes.c_ubyte),
		('num_samples', ctypes.c_ulonglong),
		('data', ctypes.POINTER(ctypes.c_ubyte))
	]

	@property
	def dtype(self):
		return 'int16' if self.fmt == b's16le' else 'uint8'

	@property
	def nbytes(self):
		return self.num_channels * self.num_samples * int(''.join(filter(str.isdigit, self.dtype))) // 8

	def __init__(self, lib):
		self.lib = lib
		self.lib.decode_audio_.restype = DecodeAudio

	def __call__(self, input_path):
		return self.lib.decode_audio_(input_path.encode())

decode_audio = DecodeAudio(ctypes.CDLL('./decode_audio.so'))
audio = decode_audio(sys.argv[1])

arr = np.ctypeslib.as_array(audio.data, shape = (audio.nbytes, )).view(audio.dtype).reshape(audio.num_samples, audio.num_channels)
print(arr.shape, arr.dtype)

arr.tofile(sys.argv[2])
