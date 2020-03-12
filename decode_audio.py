import os
import sys
import ctypes
import numpy as np

class DecodeAudio(ctypes.Structure):
	_fields_ = [
		('fmt', ctypes.c_char * 8),
		('sample_rate', ctypes.c_ulonglong),
		('num_channels', ctypes.c_ulonglong),
		('num_samples', ctypes.c_ulonglong),
		('itemsize', ctypes.c_ulonglong),
		('data', ctypes.POINTER(ctypes.c_ubyte))
	]

	@property
	def dtype(self):
		return 'int16' if self.fmt == b's16le' else 'uint8'

	@property
	def nbytes(self):
		return self.num_channels * self.num_samples * self.itemsize

	def __init__(self, lib):
		self.lib = lib
		self.lib.decode_audio.restype = DecodeAudio

	def __call__(self, input_path):
		return self.lib.decode_audio(input_path.encode())

decode_audio = DecodeAudio(ctypes.CDLL(os.path.abspath('decode_audio.so')))
audio = decode_audio(sys.argv[1])
print('ffplay', '-f', audio.fmt, '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', sys.argv[1], '# num samples:', audio.num_samples)

arr = np.ctypeslib.as_array(audio.data, shape = (audio.nbytes, )).view(audio.dtype).reshape(audio.num_samples, audio.num_channels)
arr.tofile(sys.argv[2])
