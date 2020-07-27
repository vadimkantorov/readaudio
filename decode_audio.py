import os
import sys
import ctypes

class DLDeviceType(ctypes.c_int):
	kDLCPU = 1
	kDLGPU = 2
	kDLCPUPinned = 3
	kDLOpenCL = 4
	kDLVulkan = 7
	kDLMetal = 8
	kDLVPI = 9
	kDLROCM = 10
	kDLExtDev = 12

class DLDataTypeCode(ctypes.c_uint8):
	kDLInt = 0
	kDLUInt = 1
	kDLFloat = 2
	kDLBfloat = 4

class DLDataType(ctypes.Structure):
	_fields_ = [
		('type_code', DLDataTypeCode),
		('bits', ctypes.c_uint8),
		('lanes', ctypes.c_uint16)
	]

class DLContext(ctypes.Structure):
	_fields_ = [
		('device_type', DLDeviceType),
		('device_id', ctypes.c_int)
	]

class DLTensor(ctypes.Structure):
	_fields_ = [
		('data', ctypes.c_void_p),
		('ctx', DLContext),
		('ndim', ctypes.c_int),
		('dtype', DLDataType),
		('shape', ctypes.POINTER(ctypes.c_int64)),
		('strides', ctypes.POINTER(ctypes.c_int64)),
		('byte_offset', ctypes.c_uint64)
	]

#class DLManagedTensor(ctypes.Structure):
#	_fields_ = [
#		('dl_tensor', DLTensor),
#		('manager_ctx', ctypes.c_void_p),
#		('deleter', ctypes.c_void_p)
#	]
#DLManagedTensorDeleter = ctypes.CFUNCTYPE(None, ctypes.POINTER(DLManagedTensor))
#DLManagedTensor._fields_[-1] = ('deleter', DLManagedTensorDeleter)

class DLManagedTensor(ctypes.Structure):
	_fields_ = [
		('dl_tensor', DLTensor),
		('manager_ctx', ctypes.c_void_p),
		('deleter', ctypes.CFUNCTYPE(None, ctypes.c_void_p))
	]

class DecodeAudio(ctypes.Structure):
	_fields_ = [
		('error', ctypes.c_char * 128),
		('fmt', ctypes.c_char * 8),
		('sample_rate', ctypes.c_ulonglong),
		('dl_managed_tensor', DLManagedTensor)
	]

	@property
	def dtype(self):
		return 'int16' if b's16' in self.fmt else 'float32' if b'f32' in self.fmt else 'uint8'

	@property
	def num_channels(self):
		return self.dl_managed_tensor.dl_tensor.shape[1]
	
	@property
	def num_samples(self):
		return self.dl_managed_tensor.dl_tensor.shape[0]
	
	@property
	def itemsize(self):
		return self.dl_managed_tensor.dl_tensor.dtype.lanes * self.dl_managed_tensor.dl_tensor.dtype.bits // 8;

	@property
	def nbytes(self):
		return self.num_channels * self.num_samples * self.itemsize

	@property
	def byte_order(self):
		return 'little' if b'le' in self.fmt else 'big' if b'be' in self.fmt else 'native'
	
	def __init__(self, lib_path = os.path.abspath('decode_audio_ffmpeg.so')):
		self.lib = ctypes.CDLL(lib_path)
		self.lib.decode_audio.restype = DecodeAudio

	def __call__(self, input_path):
		audio = self.lib.decode_audio(input_path.encode())
		if audio.error:
			raise Exception(audio.error.decode())
		return audio

	def __bytes__(self):
		return bytes(memoryview(ctypes.cast(self.dl_managed_tensor.dl_tensor.data, ctypes.POINTER(ctypes.c_ubyte * self.nbytes)).contents))

if __name__ == '__main__':
	decode_audio = DecodeAudio()

	audio = decode_audio(sys.argv[1])
	print('ffplay', '-f', audio.fmt, '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', sys.argv[1], '# num samples:', audio.num_samples)

	import numpy
	
	if 'numpy' in sys.argv[2]:
		array = numpy.frombuffer(bytes(audio), dtype = numpy.dtype(audio.dtype).newbyteorder(dict(little = '<', big = '>', native = '=')[audio.byte_order])).reshape(-1, audio.num_channels)

	elif 'torch' in sys.argv[2]:
		import torch
		array = torch.tensor((), dtype = getattr(torch, audio.dtype)).set_(dict(uint8 = torch.ByteStorage, int16 = torch.ShortStorage, float32 = torch.FloatStorage)[audio.dtype].from_buffer(bytes(audio), byte_order = audio.byte_order)).reshape(-1, audio.num_channels)

	print(array.dtype, array.shape)

	numpy.asarray(array).tofile(sys.argv[2])
	
	print('ffplay', '-f', audio.fmt, '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', sys.argv[2], '# num samples:', audio.num_samples)
