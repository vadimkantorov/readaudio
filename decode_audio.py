# support raw data input 
# redo API, maybe add explicit duration, maybe remove DLPack, keep only NumPy
# specify output channel_layour?

# https://bugs.python.org/issue11429
# https://bugs.python.org/issue12836
# https://stackoverflow.com/questions/20439640/ffmpeg-audio-transcoding-using-libav-libraries
# https://stackoverflow.com/questions/45549285/resampling-audio-using-libswresample-from-48000-to-44100
# https://www.codetd.com/en/article/6791150
# https://gist.github.com/jimjibone/6569303
# https://gavv.github.io/articles/decode-play/

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

	def __str__(self):
		return repr(self.descr) if len(self.descr) > 1 else self.descr[0][1]

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

	@property
	def __array_interface__(self):
		shape = tuple(self.shape[dim] for dim in range(self.ndim))
		strides = tuple(self.strides[dim] * self.itemsize for dim in range(self.ndim))
		typestr = '|' + str(self.dtype.type_code)[0] + str(self.itemsize)
		return dict(version = 3, shape = shape, strides = strides, data = (self.data, True), offset = self.byte_offset, typestr = typestr)

	def __str__(self):
		return 'dtype={dtype}, ndim={ndim}, shape={shape}, strides={strides}, byte_offset={byte_offset}'.format(dtype = self.dtype, ndim = self.ndim, shape = tuple(self.shape[i] for i in range(self.ndim)), strides = tuple(self.strides[i] for i in range(self.ndim)), byte_offset = self.byte_offset)

class DLManagedTensor(ctypes.Structure):
	_fields_ = [
		('dl_tensor', DLTensor),
		('manager_ctx', ctypes.c_void_p),
		('deleter', ctypes.CFUNCTYPE(None, ctypes.c_void_p))
	]

PyCapsule_Destructor = ctypes.CFUNCTYPE(None, ctypes.py_object)
PyCapsule_New = ctypes.pythonapi.PyCapsule_New
PyCapsule_New.restype = ctypes.py_object
PyCapsule_New.argtypes = (ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p)#PyCapsule_Destructor)
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
		('duration', ctypes.c_double),
		('itemsize', ctypes.c_ulonglong),
		('data', DLManagedTensor)
	]
	
	def __init__(self, lib_path = os.path.abspath('decode_audio_ffmpeg.so')):
		self.lib = ctypes.CDLL(lib_path)
		self.lib.decode_audio.argtypes = [ctypes.c_char_p, ctypes.c_int, DecodeAudio, DecodeAudio, ctypes.c_char_p]
		self.lib.decode_audio.restype = DecodeAudio	

	def __str__(self):
		return f'num_samples={self.num_samples}, num_channels={self.num_channels}, sample_fmt={self.fmt.decode()}, {self.data.dl_tensor}'

	def __call__(self, input_path = None, probe = False, input_buffer = None, output_buffer = None, sample_rate = None, filter_string = ''):
		uint8 = DLDataType(lanes = 1, bits = 8, code = DLDataTypeCode.kDLUInt)
		shape_single_dim = lambda buffer_len: ctypes.cast(ctypes.addressof(buffer_len), ctypes.POINTER(ctypes.c_int64))

		input_options = DecodeAudio()
		output_options = DecodeAudio()
		
		input_buffer_len = ctypes.c_int64(len(input_buffer) if input_buffer is not None else 0)
		output_buffer_len = ctypes.c_int64(len(output_buffer) if output_buffer is not None else 0)
		if input_buffer is not None:
			#input_options.data.dl_tensor.data = ctypes.cast(input_buffer, ctypes.c_void_p)
			#input_options.data.dl_tensor.data = input_buffer.ctypes.data_as(ctypes.c_void_p)
			input_options.data.dl_tensor.data = ctypes.c_void_p(input_buffer.__array_interface__['data'][0])
			
			input_options.data.dl_tensor.shape = shape_single_dim(input_buffer_len)
			input_options.data.dl_tensor.ndim = 1
			input_options.data.dl_tensor.dtype = uint8

		if output_buffer is not None:
			output_options.data.dl_tensor.data = ctypes.cast((ctypes.c_char * len(input_buffer)).from_buffer(memoryview(output_buffer)), ctypes.c_void_p) 
			output_options.data.dl_tensor.shape = shape_single_dim(output_buffer_len)
			output_options.data.dl_tensor.ndim = 1
			output_options.data.dl_tensor.dtype = uint8
		
		if sample_rate is not None:
			output_options.sample_rate = sample_rate

		audio = self.lib.decode_audio(input_path.encode() if input_path else None, probe, input_options, output_options, filter_string.encode() if filter_string else None)
		if audio.error:
			raise Exception(audio.error.decode())
		return audio
	
	def to_dlpack(self):
		byte_order = 'little' if b'le' in self.fmt else 'big' if b'be' in self.fmt else 'native'
		assert byte_order == 'native' or byte_order == sys.byteorder
		return PyCapsule_New(ctypes.byref(self.data), b'dltensor', None)

def numpy_from_dlpack(pycapsule):
	data = ctypes.cast(PyCapsule_GetPointer(pycapsule, b'dltensor'), ctypes.POINTER(DLManagedTensor)).contents
	wrapped = type('', (), dict(__array_interface__ = data.dl_tensor.__array_interface__, __del__ = lambda self: data.deleter(ctypes.byref(data)) if data.deleter else None))()
	return numpy.asarray(wrapped)
	

if __name__ == '__main__':
	import argparse
	import time
	import numpy
	import scipy.io.wavfile
	import soundfile
	import torch.utils.dlpack
	
	parser = argparse.ArgumentParser()
	parser.add_argument('--input-path', '-i', default = 'test.wav')
	parser.add_argument('--output-path', '-o', default = 'numpy.raw')
	parser.add_argument('--buffer', action = 'store_true')
	parser.add_argument('--probe', action = 'store_true')
	parser.add_argument('--sample-rate', type = int)
	parser.add_argument('--filter', default = '')#volume=volume=3.0') 
	args = parser.parse_args()
	
	def measure(k, f, audio_path, K = 100, timer = time.process_time, **kwargs):
		tic = timer()
		for i in range(K):
			audio = f(audio_path, **kwargs)
		print(k, (timer() - tic) * 1000 / K, 'msec')
		return audio

	measure('scipy.io.wavfile.read', scipy.io.wavfile.read, args.input_path)
	measure('soundfile.read', soundfile.read, args.input_path, dtype = 'int16')

	decode_audio = DecodeAudio()

	input_buffer_ = open(args.input_path, 'rb').read()
	input_buffer = numpy.frombuffer(input_buffer_, dtype = numpy.uint8)
	output_buffer = bytearray(b'\0' * 1000000) #numpy.zeros((1_000_000), dtype = numpy.uint8)
	audio = measure('ffmpeg', decode_audio, args.input_path if not args.buffer else None, input_buffer = input_buffer if args.buffer else None, output_buffer = output_buffer if args.buffer else None, filter_string = args.filter, probe = args.probe, sample_rate = args.sample_rate)

	print('ffplay', '-f', audio.fmt.decode(), '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', args.input_path, '#', audio)
	if not args.probe:
		dlpack_tensor = audio.to_dlpack()
		if 'numpy' in args.output_path:
			array = numpy_from_dlpack(dlpack_tensor)
		elif 'torch' in args.output_path:
			array = torch.utils.dlpack.from_dlpack(dlpack_tensor)

		numpy.asarray(array).tofile(args.output_path)
		print('ffplay', '-f', audio.fmt.decode(), '-ac', audio.num_channels, '-ar', audio.sample_rate, '-i', args.output_path, '# num samples:', audio.num_samples, 'dtype', array.dtype, 'shape', array.shape)
	
		del array
		del dlpack_tensor
