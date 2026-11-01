"""Extract any archive libarchive reads (rar, rar5, 7z, zip) into a directory.
usage: unarc.py <archive> <out_dir>"""
import ctypes, os, sys
lib = ctypes.CDLL('libarchive.so.13')
lib.archive_read_new.restype = ctypes.c_void_p
lib.archive_read_support_format_all.argtypes = [ctypes.c_void_p]
lib.archive_read_support_filter_all.argtypes = [ctypes.c_void_p]
lib.archive_read_open_filename.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
lib.archive_read_next_header.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
lib.archive_entry_pathname.argtypes = [ctypes.c_void_p]
lib.archive_entry_pathname.restype = ctypes.c_char_p
lib.archive_entry_filetype.argtypes = [ctypes.c_void_p]
lib.archive_read_data.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
lib.archive_read_data.restype = ctypes.c_ssize_t
lib.archive_error_string.argtypes = [ctypes.c_void_p]
lib.archive_error_string.restype = ctypes.c_char_p
lib.archive_read_free.argtypes = [ctypes.c_void_p]
src, out = sys.argv[1], sys.argv[2]
a = lib.archive_read_new()
lib.archive_read_support_format_all(a)
lib.archive_read_support_filter_all(a)
if lib.archive_read_open_filename(a, src.encode(), 1 << 16) != 0:
    sys.exit('open failed: %s' % lib.archive_error_string(a))
entry = ctypes.c_void_p()
buf = ctypes.create_string_buffer(1 << 20)
n = 0
while True:
    r = lib.archive_read_next_header(a, ctypes.byref(entry))
    if r == 1:  # ARCHIVE_EOF
        break
    if r < -20:
        sys.exit('header error: %s' % lib.archive_error_string(a))
    name = lib.archive_entry_pathname(entry).decode('utf-8', 'replace').replace('\\', '/')
    dest = os.path.abspath(os.path.join(out, name))
    if not dest.startswith(os.path.abspath(out) + os.sep):
        continue  # no escaping the output directory
    if lib.archive_entry_filetype(entry) == 0o040000:
        os.makedirs(dest, exist_ok=True)
        continue
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'wb') as fh:
        while True:
            k = lib.archive_read_data(a, buf, len(buf))
            if k < 0:
                sys.exit('data error in %s: %s' % (name, lib.archive_error_string(a)))
            if k == 0:
                break
            fh.write(buf.raw[:k])
    n += 1
lib.archive_read_free(a)
print('extracted %d files' % n)
