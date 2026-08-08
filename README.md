# GmExec
A debug tool for developers and addon creators who use garry's mod lua to run arbitrary code in the game easily.

# What does GmExec do ?
[a lot of stuff.](https://www.youtube.com/watch?v=Pz-SZlU4C10) It's open source brotato, just read the code (i'm too tired to explain)

# What is it for ?
It's in the description. But uh, it's a lua executor, it allows you to run lua code into garry's mod.
I don't really know if any addon dev or whatever uses that kind of tool but, it can, imo, really be usefull to
1. test code in game such as menus, loops, anything, i don't even have to explain
2. code anti cheats by detecting injected/executed code in the client

# What is that file ?
- compiler.py is the compiler, it's in the name. It turns the raw C script into a dll.
- when compiled and injected, source.c provides a debug console and a windows interface to run code. The ui is ugly but that's how windows is and I don't know if and how I can make it better.
- when compiled and injected, source_http.c opens a port (8080 by default) on the localhost who hosts a html (executor) at the root (http://127.0.0.1:8080) and a endpoint (http://127.0.0.1:8080/execute) that allows any program to run lua with a POST:

```html
fetch('http://127.0.0.1:8080/execute', {
  method: 'POST',
  body: 'print("Hello World!")'
});
```

# How to use it ?
- EASY: go to releases, download the latest dll and use any tool to inject it in gmod.exe
- HARDER: I'll keep it short, install [python 3.14](https://www.python.org/ftp/python/3.14.0/python-3.14.0-amd64.exe), [chocolatey](https://community.chocolatey.org/), [gcc](https://community.chocolatey.org/packages/mingw) and finally, using pip, requirements.txt. Then just run compiler.py, select source.c and it should compile the dll.

# Why is the compiler in python ?
I tried using Visual Studio many times but since I know python better and there's ways to make compilers and injectors with it, my choice was made.

# Where can I use it ?
in servers who allow the use of such tool.
