import sys
import ctypes
import numpy as np

class Audio(ctypes.Structure):
	_fields_ = [
		('fmt', ctypes.c_char * 8),
		('num_channels', ctypes.c_ubyte),
		('num_samples', ctypes.c_ulonglong),
		('data', ctypes.c_void_p)
	]

lib = ctypes.cdll.LoadLibrary('./decode_audio.so')
lib.decode_audio_.restype = Audio

audio = lib.decode_audio_(sys.argv[1].encode('ascii'))

#shape = (100, )
#arr = numpy.ctypeslib.as_array(pointer, shape)

#arr.tofile(sys.argv[2])
