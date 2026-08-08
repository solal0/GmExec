# GmExec
A debug tool for developers and addon creators who use garry's mod lua to run arbitrary code in the game easily.

# What is it for ?
It's in the description. But uh, it's a lua executor, it allows you to run lua code into garry's mod.
I don't really know if any addon dev or whatever uses that kind of tool but, it can, imo, really be usefull to
1. test code in game such as menus, loops, anything, i don't even have to explain
2. code anti cheats by detecting injected/executed code in the client

# How to use it ?
- EASY: go to releases, download the latest dll and use any tool to inject it in gmod.exe
- HARDER: I'll keep it short, install [python 3.14](https://www.python.org/ftp/python/3.14.0/python-3.14.0-amd64.exe), [chocolatey](https://community.chocolatey.org/), [gcc](https://community.chocolatey.org/packages/mingw) and finally, using pip, requirements.txt. Then just run compiler.py, select source.c and it should compile the dll.

# Where can I use it ?
in servers who allow the use of such tool.
