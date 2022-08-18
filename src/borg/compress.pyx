"""
borg.compress
=============

Compression is applied to chunks after ID hashing (so the ID is a direct function of the
plain chunk, compression is irrelevant to it), and of course before encryption.

The "auto" mode (e.g. --compression auto,lzma,4) is implemented as a meta Compressor,
meaning that Auto acts like a Compressor, but defers actual work to others (namely
LZ4 as a heuristic whether compression is worth it, and the specified Compressor
for the actual compression).

Decompression is normally handled through Compressor.decompress which will detect
which compressor has been used to compress the data and dispatch to the correct
decompressor.
"""

import random
from struct import Struct
import zlib

try:
    import lzma
except ImportError:
    lzma = None


from .constants import MAX_DATA_SIZE
from .helpers import Buffer, DecompressionError

API_VERSION = '1.2_02'

cdef extern from "lz4.h":
    int LZ4_compress_default(const char* source, char* dest, int inputSize, int maxOutputSize) nogil
    int LZ4_decompress_safe(const char* source, char* dest, int inputSize, int maxOutputSize) nogil
    int LZ4_compressBound(int inputSize) nogil


cdef extern from "zstd.h":
    size_t ZSTD_compress(void* dst, size_t dstCapacity, const void* src, size_t srcSize, int  compressionLevel) nogil
    size_t ZSTD_decompress(void* dst, size_t dstCapacity, const void* src, size_t compressedSize) nogil
    size_t ZSTD_compressBound(size_t srcSize) nogil
    unsigned long long ZSTD_CONTENTSIZE_UNKNOWN
    unsigned long long ZSTD_CONTENTSIZE_ERROR
    unsigned long long ZSTD_getFrameContentSize(const void *src, size_t srcSize) nogil
    unsigned ZSTD_isError(size_t code) nogil
    const char* ZSTD_getErrorName(size_t code) nogil


buffer = Buffer(bytearray, size=0)


cdef class CompressorBase:
    """
    base class for all (de)compression classes,
    also handles compression format auto detection and
    adding/stripping the ID header (which enable auto detection).
    """
    ID = 0xFF  # reserved and not used
               # overwrite with a unique 1-byte bytestring in child classes
    name = 'baseclass'

    cheader_fmt = Struct('<BBI')  # compressor ID byte, level byte, len(plaintext) int

    @classmethod
    def detect(cls, data):
        return data[0] == cls.ID

    def __init__(self, level=255, **kwargs):
        assert 0 <= level <= 255
        self.level = level

    def decide(self, data):
        """
        Return which compressor will perform the actual compression for *data*.

        This exists for a very specific case: If borg recreate is instructed to recompress
        using Auto compression it needs to determine the _actual_ target compression of a chunk
        in order to detect whether it should be recompressed.

        Any compressor may return a compressor other than *self*, like e.g. the CNONE compressor,
        and should actually do so if *data* would become larger on compression.
        """
        return self

    def make_hdr(self, length):
        """Generate the compression header."""
        return self.cheader_fmt.pack(self.ID, self.level, length)

    def parse_hdr(self, data):
        """Parse the compression header."""
        return self.cheader_fmt.unpack(data[:self.cheader_fmt.size])  # ID, level, length

    def get_payload(self, data):
        """Return the payload (part after the header)."""
        return data[self.cheader_fmt.size:]

    def compress(self, data):
        """
        Compress *data* (bytes) and return bytes result. Prepend the header bytes of this compressor,
        which are needed so that the correct decompressor can be used for decompression.
        """
        raise NotImplemented

    def decompress(self, data):
        """
        Decompress *data* (preferably a memoryview, bytes also acceptable) and return bytes result.
        The leading header bytes need to be present.
        """
        raise NotImplemented


cdef class DecidingCompressor(CompressorBase):
    """
    base class for (de)compression classes that (based on an internal _decide
    method) decide whether and how to compress data.
    """
    name = 'decidebaseclass'

    def __init__(self, level=255, **kwargs):
        super().__init__(level=level, **kwargs)

    def _decide(self, data):
        """
        Decides what to do with *data*. Returns (compressor, compressed_data).

        *compressed_data* can be the result of *data* being processed by *compressor*,
        if that is generated as a side-effect of the decision process, or None otherwise.

        This private method allows for more efficient implementation of compress()
        and decide_compress() making use of *compressed_data*, if already generated.
        """
        raise NotImplementedError

    def decide(self, data):
        return self._decide(data)[0]

    def decide_compress(self, data):
        """
        Decides what to do with *data* and handle accordingly. Returns (compressor, compressed_data).

        *compressed_data* is the result of *data* being processed by *compressor*.
        """
        length = len(data)
        compressor, compressed_data = self._decide(data)

        if compressed_data is None:
            compressed_data = compressor.compress(data)

        if compressor is self:
            return self, self.make_hdr(length) + compressed_data

        return compressor, compressed_data

    def compress(self, data):
        return self.decide_compress(data)[1]

class CNONE(CompressorBase):
    """
    none - no compression, just pass through data
    """
    ID = 0x00
    name = 'none'

    def __init__(self, level=255, **kwargs):
        super().__init__(level=level, **kwargs)  # no defined levels for CNONE, so just say "unknown"

    def compress(self, data):
        return self.make_hdr(len(data)) + data

    def decompress(self, data):
        id, level, length = self.parse_hdr(data)
        data = self.get_payload(data)
        if not isinstance(data, bytes):
            data = bytes(data)
        assert len(data) == length
        return data


class LZ4(DecidingCompressor):
    """
    raw LZ4 compression / decompression (liblz4).

    Features:
        - lz4 is super fast
        - wrapper releases CPython's GIL to support multithreaded code
        - uses safe lz4 methods that never go beyond the end of the output buffer
    """
    ID = 0x01
    name = 'lz4'

    def __init__(self, level=255, **kwargs):
        super().__init__(level=level, **kwargs)  # no defined levels for LZ4, so just say "unknown"

    def _decide(self, idata):
        """
        Decides what to do with *data*. Returns (compressor, lz4_data).

        *lz4_data* is the LZ4 result if *compressor* is LZ4 as well, otherwise it is None.
        """
        if not isinstance(idata, bytes):
            idata = bytes(idata)  # code below does not work with memoryview
        cdef int isize = len(idata)
        cdef int osize
        cdef char *source = idata
        cdef char *dest
        osize = LZ4_compressBound(isize)
        buf = buffer.get(osize)
        dest = <char *> buf
        with nogil:
            osize = LZ4_compress_default(source, dest, isize, osize)
        if not osize:
            raise Exception('lz4 compress failed')
        # only compress if the result actually is smaller
        if osize < isize:
            return self, dest[:osize]
        else:
            return NONE_COMPRESSOR, None

    def decompress(self, data):
        id, level, length = self.parse_hdr(data)
        idata = self.get_payload(data)
        if not isinstance(idata, bytes):
            idata = bytes(idata)  # code below does not work with memoryview
        cdef int isize = len(idata)
        cdef int osize = length
        cdef int rsize
        cdef char *source = idata
        cdef char *dest
        try:
            buf = buffer.get(osize)
        except MemoryError:
            raise DecompressionError('MemoryError')
        dest = <char *> buf
        with nogil:
            rsize = LZ4_decompress_safe(source, dest, isize, osize)
        if rsize < 0:
            raise DecompressionError('lz4 decompress failed')
        assert rsize == length
        return dest[:rsize]


class LZMA(DecidingCompressor):
    """
    lzma compression / decompression
    """
    ID = 0x02
    name = 'lzma'

    def __init__(self, level=6, **kwargs):
        super().__init__(level=level, **kwargs)
        self.level = level
        if lzma is None:
            raise ValueError('No lzma support found.')

    def _decide(self, data):
        """
        Decides what to do with *data*. Returns (compressor, lzma_data).

        *lzma_data* is the LZMA result if *compressor* is LZMA as well, otherwise it is None.
        """
        # we do not need integrity checks in lzma, we do that already
        lzma_data = lzma.compress(data, preset=self.level, check=lzma.CHECK_NONE)
        if len(lzma_data) < len(data):
            return self, lzma_data
        else:
            return NONE_COMPRESSOR, None

    def decompress(self, data):
        id, level, length = self.parse_hdr(data)
        data = self.get_payload(data)
        try:
            uncompressed = lzma.decompress(data)
            assert len(uncompressed) == length
            return uncompressed
        except lzma.LZMAError as e:
            raise DecompressionError(str(e)) from None


class ZSTD(DecidingCompressor):
    """zstd compression / decompression (pypi: zstandard, gh: python-zstandard)"""
    # This is a NOT THREAD SAFE implementation.
    # Only ONE python context must be created at a time.
    # It should work flawlessly as long as borg will call ONLY ONE compression job at time.
    ID = 0x03
    name = 'zstd'

    def __init__(self, level=3, **kwargs):
        super().__init__(level=level, **kwargs)
        self.level = level

    def _decide(self, idata):
        """
        Decides what to do with *data*. Returns (compressor, zstd_data).

        *zstd_data* is the ZSTD result if *compressor* is ZSTD as well, otherwise it is None.
        """
        if not isinstance(idata, bytes):
            idata = bytes(idata)  # code below does not work with memoryview
        cdef int isize = len(idata)
        cdef int osize
        cdef char *source = idata
        cdef char *dest
        cdef int level = self.level
        osize = ZSTD_compressBound(isize)
        buf = buffer.get(osize)
        dest = <char *> buf
        with nogil:
            osize = ZSTD_compress(dest, osize, source, isize, level)
        if ZSTD_isError(osize):
            raise Exception('zstd compress failed: %s' % ZSTD_getErrorName(osize))
        # only compress if the result actually is smaller
        if osize < isize:
            return self, dest[:osize]
        else:
            return NONE_COMPRESSOR, None

    def decompress(self, data):
        id, level, length = self.parse_hdr(data)
        idata = self.get_payload(data)
        if not isinstance(idata, bytes):
            idata = bytes(idata)  # code below does not work with memoryview
        cdef int isize = len(idata)
        cdef unsigned long long osize
        cdef unsigned long long rsize
        cdef char *source = idata
        cdef char *dest
        osize = ZSTD_getFrameContentSize(source, isize)
        if osize == ZSTD_CONTENTSIZE_ERROR:
            raise DecompressionError('zstd get size failed: data was not compressed by zstd')
        if osize == ZSTD_CONTENTSIZE_UNKNOWN:
            raise DecompressionError('zstd get size failed: original size unknown')
        try:
            buf = buffer.get(osize)
        except MemoryError:
            raise DecompressionError('MemoryError')
        dest = <char *> buf
        with nogil:
            rsize = ZSTD_decompress(dest, osize, source, isize)
        if ZSTD_isError(rsize):
            raise DecompressionError('zstd decompress failed: %s' % ZSTD_getErrorName(rsize))
        if rsize != osize:
            raise DecompressionError('zstd decompress failed: size mismatch')
        assert osize == length
        return dest[:osize]


class ZLIB(DecidingCompressor):
    """
    zlib compression / decompression (python stdlib)
    """
    ID = 0x05
    name = 'zlib'

    def __init__(self, level=6, **kwargs):
        super().__init__(level=level, **kwargs)
        self.level = level

    def _decide(self, data):
        """
        Decides what to do with *data*. Returns (compressor, zlib_data).

        *zlib_data* is the ZLIB result if *compressor* is ZLIB as well, otherwise it is None.
        """
        zlib_data = zlib.compress(data, self.level)
        if len(zlib_data) < len(data):
            return self, zlib_data
        else:
            return NONE_COMPRESSOR, None

    def decompress(self, data):
        id, level, length = self.parse_hdr(data)
        data = self.get_payload(data)
        try:
            uncompressed = zlib.decompress(data)
            assert len(uncompressed) == length
            return uncompressed
        except zlib.error as e:
            raise DecompressionError(str(e)) from None


class ZLIB_legacy(CompressorBase):
    """
    zlib compression / decompression (python stdlib)

    Note: This is the legacy ZLIB support as used by borg < 1.3.
          It still suffers from attic *only* supporting zlib and not having separate
          ID bytes to differentiate between differently compressed chunks.
          This just works because zlib compressed stuff always starts with 0x.8.. bytes.
          Newer borg uses the ZLIB class that has separate ID bytes (as all the other
          compressors) and does not need this hack.
    """
    ID = 0x08  # not used here, see detect()
    # avoid all 0x.8 IDs elsewhere!
    name = 'zlib_legacy'

    @classmethod
    def detect(cls, data):
        # matches misc. patterns 0x.8.. used by zlib
        cmf, flg = data[:2]
        is_deflate = cmf & 0x0f == 8
        check_ok = (cmf * 256 + flg) % 31 == 0
        return check_ok and is_deflate

    def __init__(self, level=6, **kwargs):
        super().__init__(level=level, **kwargs)
        self.level = level

    def compress(self, data):
        # note: for compatibility no super call, do not add ID bytes
        return zlib.compress(data, self.level)

    def decompress(self, data):
        # note: legacy zlib has no header!
        try:
            return zlib.decompress(data)
        except zlib.error as e:
            raise DecompressionError(str(e)) from None


class Auto(CompressorBase):
    """
    Meta-Compressor that decides which compression to use based on LZ4's ratio.

    As a meta-Compressor the actual compression is deferred to other Compressors,
    therefore this Compressor has no ID, no detect() and no decompress().
    """

    ID = None
    name = 'auto'

    def __init__(self, compressor):
        super().__init__()
        self.compressor = compressor

    def _decide(self, data):
        """
        Decides what to do with *data*. Returns (compressor, compressed_data).

        *compressor* is the compressor that is decided to be best suited to compress
        *data*, *compressed_data* is the result of *data* being compressed by a
        compressor, which may or may not be *compressor*!

        There are three possible outcomes of the decision process:
        * *data* compresses well enough for trying the more expensive compressor
          set on instantiation to make sense.
          In this case, (expensive_compressor_class, lz4_compressed_data) is
          returned.
        * *data* compresses only slightly using the LZ4 compressor, thus trying
          the more expensive compressor for potentially little gain does not
          make sense.
          In this case, (LZ4_COMPRESSOR, lz4_compressed_data) is returned.
        * *data* does not compress at all using LZ4, in this case
          (NONE_COMPRESSOR, none_compressed_data) is returned.

        Note: While it makes no sense, the expensive compressor may well be set
        to the LZ4 compressor.
        """
        compressor, compressed_data = LZ4_COMPRESSOR.decide_compress(data)
        # compressed_data includes the compression type header, while data does not yet
        ratio = len(compressed_data) / (len(data) + 2)
        if ratio < 0.97:
            return self.compressor, compressed_data
        else:
            return compressor, compressed_data

    def decide(self, data):
        return self._decide(data)[0]

    def compress(self, data):
        compressor, cheap_compressed_data = self._decide(data)
        if compressor in (LZ4_COMPRESSOR, NONE_COMPRESSOR):
            # we know that trying to compress with expensive compressor is likely pointless,
            # so we fallback to return the cheap compressed data.
            return cheap_compressed_data
        # if we get here, the decider decided to try the expensive compressor.
        # we also know that the compressed data returned by the decider is lz4 compressed.
        expensive_compressed_data = compressor.compress(data)
        ratio = len(expensive_compressed_data) / len(cheap_compressed_data)
        if ratio < 0.99:
            # the expensive compressor managed to squeeze the data significantly better than lz4.
            return expensive_compressed_data
        else:
            # otherwise let's just store the lz4 data, which decompresses extremely fast.
            return cheap_compressed_data

    def decompress(self, data):
        raise NotImplementedError

    def detect(cls, data):
        raise NotImplementedError


class ObfuscateSize(CompressorBase):
    """
    Meta-Compressor that obfuscates the compressed data size.
    """
    ID = 0x04
    name = 'obfuscate'

    oheader_fmt = Struct('<I')

    def __init__(self, level=None, compressor=None):
        super().__init__(level=level)  # data will be encrypted, so we can tell the level
        self.compressor = compressor
        if level is None:
            pass  # decompression
        elif 1 <= level <= 6:
            self._obfuscate = self._relative_random_reciprocal_obfuscate
            self.factor = 0.001 * 10 ** level
            self.min_r = 0.0001
        elif 110 <= level <= 123:
            self._obfuscate = self._random_padding_obfuscate
            self.max_padding_size = 2 ** (level - 100)  # 1kiB .. 8MiB

    def _obfuscate(self, compr_size):
        # implementations need to return the size of obfuscation data,
        # that the caller shall add.
        raise NotImplemented

    def _relative_random_reciprocal_obfuscate(self, compr_size):
        # effect for SPEC 1:
        # f = 0.01 .. 0.1 for r in 1.0 .. 0.1 == in 90% of cases
        # f = 0.1 .. 1.0 for r in 0.1 .. 0.01 == in 9% of cases
        # f = 1.0 .. 10.0 for r in 0.01 .. 0.001 = in 0.9% of cases
        # f = 10.0 .. 100.0 for r in 0.001 .. 0.0001 == in 0.09% of cases
        r = max(self.min_r, random.random())  # 0..1, but don't get too close to 0
        f = self.factor / r
        return int(compr_size * f)

    def _random_padding_obfuscate(self, compr_size):
        return int(self.max_padding_size * random.random())

    def compress(self, data):
        length = len(data)
        compressed_data = self.compressor.compress(data)  # compress data
        compr_size = len(compressed_data)
        oheader = self.oheader_fmt.pack(compr_size)
        addtl_size = self._obfuscate(compr_size)
        addtl_size = max(0, addtl_size)  # we can only make it longer, not shorter!
        addtl_size = min(MAX_DATA_SIZE - 1024 - compr_size, addtl_size)  # stay away from MAX_DATA_SIZE
        trailer = bytes(addtl_size)
        return b''.join([self.make_hdr(length), oheader, compressed_data, trailer])

    def decompress(self, data):
        id, level, length = self.parse_hdr(data)
        obfuscated_data = self.get_payload(data)
        compr_size = self.oheader_fmt.unpack(obfuscated_data[0:self.oheader_fmt.size])[0]
        compressed_data = obfuscated_data[self.oheader_fmt.size:self.oheader_fmt.size+compr_size]
        compressor_cls = Compressor.detect(compressed_data)[0]
        compressor = compressor_cls()
        return compressor.decompress(compressed_data)


# Maps valid compressor names to their class
COMPRESSOR_TABLE = {
    CNONE.name: CNONE,
    LZ4.name: LZ4,
    ZLIB.name: ZLIB,
    ZLIB_legacy.name: ZLIB_legacy,
    LZMA.name: LZMA,
    Auto.name: Auto,
    ZSTD.name: ZSTD,
    ObfuscateSize.name: ObfuscateSize,
}
# List of possible compression types. Does not include Auto, since it is a meta-Compressor.
COMPRESSOR_LIST = [LZ4, ZSTD, CNONE, ZLIB, ZLIB_legacy, LZMA, ObfuscateSize, ]  # check fast stuff first

def get_compressor(name, **kwargs):
    cls = COMPRESSOR_TABLE[name]
    return cls(**kwargs)

# compressor instances to be used by all other compressors
NONE_COMPRESSOR = get_compressor('none')
LZ4_COMPRESSOR = get_compressor('lz4')

class Compressor:
    """
    compresses using a compressor with given name and parameters
    decompresses everything we can handle (autodetect)
    """
    def __init__(self, name='null', **kwargs):
        self.params = kwargs
        self.compressor = get_compressor(name, **self.params)

    def compress(self, data):
        return self.compressor.compress(data)

    def decompress(self, data):
        compressor_cls = self.detect(data)[0]
        return compressor_cls(**self.params).decompress(data)

    @staticmethod
    def detect(data):
        hdr = bytes(data[:2])  # detect() does not work with memoryview
        level = hdr[1]  # usually the level, but not for zlib_legacy
        for cls in COMPRESSOR_LIST:
            if cls.detect(hdr):
                return cls, (255 if cls.name == 'zlib_legacy' else level)
        else:
            raise ValueError('No decompressor for this data found: %r.', data[:2])


class CompressionSpec:
    def __init__(self, s):
        values = s.split(',')
        count = len(values)
        if count < 1:
            raise ValueError
        # --compression algo[,level]
        self.name = values[0]
        if self.name in ('none', 'lz4', ):
            return
        elif self.name in ('zlib', 'lzma', 'zlib_legacy'):  # zlib_legacy just for testing
            if count < 2:
                level = 6  # default compression level in py stdlib
            elif count == 2:
                level = int(values[1])
                if not 0 <= level <= 9:
                    raise ValueError
            else:
                raise ValueError
            self.level = level
        elif self.name in ('zstd', ):
            if count < 2:
                level = 3  # default compression level in zstd
            elif count == 2:
                level = int(values[1])
                if not 1 <= level <= 22:
                    raise ValueError
            else:
                raise ValueError
            self.level = level
        elif self.name == 'auto':
            if 2 <= count <= 3:
                compression = ','.join(values[1:])
            else:
                raise ValueError
            self.inner = CompressionSpec(compression)
        elif self.name == 'obfuscate':
            if 3 <= count <= 5:
                level = int(values[1])
                if not ((1 <= level <= 6) or (110 <= level <= 123)):
                    raise ValueError
                self.level = level
                compression = ','.join(values[2:])
            else:
                raise ValueError
            self.inner = CompressionSpec(compression)
        else:
            raise ValueError

    @property
    def compressor(self):
        if self.name in ('none', 'lz4', ):
            return get_compressor(self.name)
        elif self.name in ('zlib', 'lzma', 'zstd', 'zlib_legacy'):
            return get_compressor(self.name, level=self.level)
        elif self.name == 'auto':
            return get_compressor(self.name, compressor=self.inner.compressor)
        elif self.name == 'obfuscate':
            return get_compressor(self.name, level=self.level, compressor=self.inner.compressor)
