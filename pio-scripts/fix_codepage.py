import locale

# Monkey-patch locale.getpreferredencoding to return utf-8
locale.getpreferredencoding = lambda do_setlocale=True: "utf-8"