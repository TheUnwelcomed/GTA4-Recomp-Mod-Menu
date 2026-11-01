"""List or extract an archive (rar, 7z, zip, tar...) with the system libarchive.

usage: python3 -I arx.py <archive> [<out_dir>]
  Without out_dir it lists the entries. With it, it writes the regular files
  under out_dir. Entry names are sanitised: no absolute paths, no '..', no
  links, so nothing lands outside out_dir.
"""
import ctypes
import ctypes.util
import os
import sys

lib = ctypes.CDLL(ctypes.util.find_library('archive') or 'libarchive.so.13')
lib.archive_read_new.restype = ctypes.c_void_p
lib.archive_read_support_format_all.argtypes = [ctypes.c_void_p]
lib.archive_read_support_filter_all.argtypes = [ctypes.c_void_p]
lib.archive_read_open_filename.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
lib.archive_read_next_header.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
lib.archive_entry_pathname_utf8.argtypes = [ctypes.c_void_p]
lib.archive_entry_pathname_utf8.restype = ctypes.c_char_p
lib.archive_entry_pathname.argtypes = [ctypes.c_void_p]
lib.archive_entry_pathname.restype = ctypes.c_char_p
lib.archive_entry_filetype.argtypes = [ctypes.c_void_p]
lib.archive_entry_filetype.restype = ctypes.c_uint
lib.archive_entry_size.argtypes = [ctypes.c_void_p]
lib.archive_entry_size.restype = ctypes.c_int64
lib.archive_read_data.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
lib.archive_read_data.restype = ctypes.c_ssize_t
lib.archive_read_data_skip.argtypes = [ctypes.c_void_p]
lib.archive_read_data_block.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p),
                                        ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_int64)]
lib.archive_error_string.argtypes = [ctypes.c_void_p]
lib.archive_error_string.restype = ctypes.c_char_p
lib.archive_read_free.argtypes = [ctypes.c_void_p]

AE_IFREG = 0o100000
ARCHIVE_EOF, ARCHIVE_OK, ARCHIVE_WARN = 1, 0, -20


def safe_name(name):
    parts = [p for p in name.replace('\\', '/').split('/') if p not in ('', '.')]
    if any(p == '..' for p in parts):
        return None
    return os.path.join(*parts) if parts else None


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else None
    a = lib.archive_read_new()
    lib.archive_read_support_format_all(a)
    lib.archive_read_support_filter_all(a)
    if lib.archive_read_open_filename(a, path.encode(), 1 << 20) != ARCHIVE_OK:
        raise SystemExit('open: %s' % lib.archive_error_string(a))
    entry = ctypes.c_void_p()
    buf = ctypes.create_string_buffer(1 << 20)
    total = count = 0
    while True:
        r = lib.archive_read_next_header(a, ctypes.byref(entry))
        if r == ARCHIVE_EOF:
            break
        if r < ARCHIVE_WARN:
            raise SystemExit('header: %s' % lib.archive_error_string(a))
        raw = lib.archive_entry_pathname_utf8(entry) or lib.archive_entry_pathname(entry) or b''
        name = raw.decode('utf-8', 'replace')
        ftype = lib.archive_entry_filetype(entry)
        size = lib.archive_entry_size(entry)
        if ftype != AE_IFREG:
            lib.archive_read_data_skip(a)
            continue
        print('%12d  %s' % (size, name))
        count += 1
        total += size
        rel = safe_name(name)
        if not out or rel is None:
            lib.archive_read_data_skip(a)
            continue
        dest = os.path.join(out, rel)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        # Block by block, so a check that fails at the end (a CRC) still
        # leaves every block that was decoded on disk, reported, not lost.
        ptr, size_out, offset = ctypes.c_void_p(), ctypes.c_size_t(), ctypes.c_int64()
        written = 0
        with open(dest, 'wb') as f:
            while True:
                r = lib.archive_read_data_block(a, ctypes.byref(ptr), ctypes.byref(size_out), ctypes.byref(offset))
                if r == ARCHIVE_EOF:
                    break
                if r < ARCHIVE_OK:
                    print('  ! %s after %d of %d bytes: %s' % (name, written, size, lib.archive_error_string(a)))
                    break
                if size_out.value:
                    f.seek(offset.value)
                    f.write(ctypes.string_at(ptr, size_out.value))
                    written = max(written, offset.value + size_out.value)
    lib.archive_read_free(a)
    print('%d files, %d bytes' % (count, total))


if __name__ == '__main__':
    main()
