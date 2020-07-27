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

	def __str__(self):
		return {self.kDLInt : 'int', self.kDLUInt : 'uint', self.kDLFloat : 'float', self.kDLBfloat : 'bfloat'}[self.value]

class DLDataType(ctypes.Structure):
	_fields_ = [
		('type_code', DLDataTypeCode),
		('bits', ctypes.c_uint8),
		('lanes', ctypes.c_uint16)
	]

	@property
	def descr(self):
		typestr = str(self.type_code) + str(self.bits)
		return [('f' + str(l), typestr) for l in range(self.lanes)]

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

	@property
	def size(self):
		prod = 1
		for i in range(self.ndim):
			prod *= self.shape[i]
		return prod

	@property
	def itemsize(self):
		return self.dtype.lanes * self.dtype.bits // 8;

	@property
	def nbytes(self):
		return self.size * self.itemsize 

class DLManagedTensor(ctypes.Structure):
	_fields_ = [
		('dl_tensor', DLTensor),
		('manager_ctx', ctypes.c_void_p),
		('deleter', ctypes.CFUNCTYPE(None, ctypes.c_void_p))
	]

PyCapsule_New = ctypes.pythonapi.PyCapsule_New
PyCapsule_New.restype = ctypes.py_object
PyCapsule_New.argtypes = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)

PyCapsule_GetPointer = ctypes.pythonapi.PyCapsule_GetPointer
PyCapsule_GetPointer.restype = ctypes.c_void_p
PyCapsule_GetPointer.argtypes = (ctypes.py_object, ctypes.c_char_p)

class DecodeAudio(ctypes.Structure):
	_fields_ = [
		('error', ctypes.c_char * 128),
		('fmt', ctypes.c_char * 8),
		('sample_rate', ctypes.c_ulonglong),
		('num_channels', ctypes.c_ulonglong),
		('num_samples', ctypes.c_ulonglong),
		('itemsize', ctypes.c_ulonglong),
		('data', DLManagedTensor)
	]

	@property
	def dtype(self):
		return 'int16' if b's16' in self.fmt else 'float32' if b'f32' in self.fmt else 'uint8'

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
		return bytes(memoryview(ctypes.cast(self.data.dl_tensor.data, ctypes.POINTER(ctypes.c_ubyte * self.nbytes)).contents))

	def to_dlpack(self):
		assert self.byte_order == 'native' or self.byte_order == sys.byteorder
		return PyCapsule_New(ctypes.byref(self.data), b'dltensor', None)

	def free(self):
		#TODO: https://stackoverflow.com/questions/37988849/safer-way-to-expose-a-c-allocated-memory-buffer-using-numpy-ctypes
		self.data.deleter(ctypes.byref(self.data))

def numpy_from_dlpack(pycapsule):
	dl_managed_tensor = ctypes.cast(PyCapsule_GetPointer(pycapsule, b'dltensor'), ctypes.POINTER(DLManagedTensor)).contents
	shape = [dl_managed_tensor.dl_tensor.shape[dim] for dim in range(dl_managed_tensor.dl_tensor.ndim)]
	#buffer = bytes(memoryview(ctypes.cast(dl_managed_tensor.dl_tensor.data, ctypes.POINTER(ctypes.c_ubyte * dl_managed_tensor.dl_tensor.nbytes)).contents))
	#return numpy.frombuffer(buffer, dtype = dl_managed_tensor.dl_tensor.dtype.descr).reshape(shape)

	pointer = ctypes.cast(dl_managed_tensor.dl_tensor.data, ctypes.POINTER(ctypes.c_ubyte * dl_managed_tensor.dl_tensor.itemsize))
	return numpy.ctypeslib.as_array(pointer, shape = shape).view(dl_managed_tensor.dl_tensor.dtype.descr)

if __name__ == '__main__':
	import numpy
	try:
		import torch
		import torch.utils.dlpack
	except:
		pass
	
	decode_audio = DecodeAudio()

	audio = decode_audio(sys.argv[1])
	print('ffplay', '-f', audio.fmt, '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', sys.argv[1], '# num samples:', audio.num_samples)

	dlpack_tensor = audio.to_dlpack()

	if 'numpy' in sys.argv[2]:
		array = numpy_from_dlpack(dlpack_tensor)
	elif 'torch' in sys.argv[2]:
		array = torch.utils.dlpack.from_dlpack(dlpack_tensor)

	print(array.dtype, array.shape)
	numpy.asarray(array).tofile(sys.argv[2])
	
	print('ffplay', '-f', audio.fmt, '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', sys.argv[2], '# num samples:', audio.num_samples)
