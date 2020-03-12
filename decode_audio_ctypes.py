import sys
import ctypes
import numpy as np

lib = ctypes.cdll.LoadLibrary('./decode_audio.so')

# does not work yet
pointer = lib.decode_audio(sys.argv[1])
shape = (100, )
arr = numpy.ctypeslib.as_array(pointer, shape)

arr.tofile(sys.argv[2])
