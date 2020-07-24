import os
import sys
import ctypes

class DecodeAudio(ctypes.Structure):
	_fields_ = [
		('error', ctypes.c_char * 128),
		('fmt', ctypes.c_char * 8),
		('sample_rate', ctypes.c_ulonglong),
		('num_channels', ctypes.c_ulonglong),
		('num_samples', ctypes.c_ulonglong),
		('itemsize', ctypes.c_ulonglong),
		('data', ctypes.POINTER(ctypes.c_ubyte))
	]

	@property
	def dtype(self):
		return 'int16' if b's16' in self.fmt else 'float32' if 'f32' in self.fmt else 'uint8'

	@property
	def nbytes(self):
		return self.num_channels * self.num_samples * self.itemsize

	@property
	def byte_order(self):
		return 'little' if b'le' in self.fmt else 'big' if b'be' in self.fmt else 'native'

	def __bytes__(self):
		return ctypes.cast(self.data, ctypes.POINTER(ctypes.c_ubyte * self.nbytes)).contents
	
	def __init__(self, lib_path = os.path.abspath('decode_audio_ffmpeg.so')):
		self.lib = ctypes.CDLL(lib_path)
		self.lib.decode_audio.restype = DecodeAudio

	def __call__(self, input_path):
		audio = self.lib.decode_audio(input_path.encode())
		if audio.error:
			raise Exception(audio.error.decode())
		return audio

if __name__ == '__main__':
	decode_audio = DecodeAudio()

	audio = decode_audio(sys.argv[1])
	print('ffplay', '-f', audio.fmt, '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', sys.argv[1], '# num samples:', audio.num_samples)

	import numpy
	
	if 'numpy' in sys.argv[2]:
		array = numpy.ctypeslib.as_array(audio.data, shape = (audio.nbytes, )).view(audio.dtype).reshape(-1, audio.num_channels)

	elif 'torch' in sys.argv[2]:
		import torch
		array = torch.tensor((), dtype = getattr(torch, audio.dtype)).set_(dict(torch.uint8 = torch.ByteStorage, int16 = torch.ShortStorage, float32 = torch.FloatStorage)[audio.dtype].from_buffer(bytes(audio), byte_order = audio.byte_order)).reshape(-1, audio.num_channels)

	print(array.dtype, array.shape)
	numpy.asarray(array).tofile(sys.argv[2])
