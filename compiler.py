import subprocess
import shutil
import sys
import os
import platform
import tkinter as tk
from tkinter import filedialog, simpledialog

def check_gcc():
    if shutil.which("gcc") is None:
        print("[!] gcc not found in PATH.")
        sys.exit(1)

def select_source_file():
    root = tk.Tk()
    root.withdraw()
    file_path = filedialog.askopenfilename(
        title="Select C source file",
        filetypes=[("C Source Files", "*.c"), ("All Files", "*.*")]
    )
    root.destroy()
    return file_path

def select_output_path(source_file):
    root = tk.Tk()
    root.withdraw()
    default_name = os.path.splitext(os.path.basename(source_file))[0] + ".dll"
    output_path = filedialog.asksaveasfilename(
        title="Save DLL as",
        defaultextension=".dll",
        initialfile=default_name,
        filetypes=[("DLL Files", "*.dll"), ("All Files", "*.*")]
    )
    root.destroy()
    return output_path

def compile_file(source_file, output_dll):
    is_64bit = platform.architecture()[0] == '64bit'
    print(f"[*] Compiling for {'x64' if is_64bit else 'x86'}")

    cmd = [
        "gcc",
        "-shared",
        "-o", output_dll,
        source_file,
        "-static-libgcc",
        "-static-libstdc++",
        "-O2",
        "-w",
        "-D_WIN32_WINNT=0x0601",
        "-D_WIN32",
        # Windows libraries
        "-luser32", "-lkernel32", "-lgdi32", "-lcomdlg32", "-lcomctl32",
        "-lpsapi", "-lwinmm", "-lws2_32", "-lole32", "-loleaut32",
        "-luuid", "-lshell32", "-ladvapi32"
    ]
    
    if is_64bit:
        cmd.extend(["-m64"])
    else:
        cmd.extend(["-m32"])

    print(f"[*] Compiling: {os.path.basename(source_file)} -> {os.path.basename(output_dll)}")
    print(f"[*] Command: {' '.join(cmd)}")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode == 0:
        print(f"[+] Success! DLL created: {os.path.abspath(output_dll)}")
        print(f"[*] Size: {os.path.getsize(output_dll)} bytes")
        return True
    else:
        print("[!] Compilation failed:")
        print(result.stderr)
        # Try minimal
        print("\n[*] Trying minimal compilation...")
        minimal = ["gcc", "-shared", "-o", output_dll, source_file, "-luser32"]
        if is_64bit:
            minimal.append("-m64")
        result2 = subprocess.run(minimal, capture_output=True, text=True)
        if result2.returncode == 0:
            print(f"[+] Success! DLL: {os.path.abspath(output_dll)}")
            return True
        else:
            print(result2.stderr)
            return False

def main():
    print("========================================")
    print("   Universal C to DLL Compiler v1.0")
    print("========================================")
    
    check_gcc()
    
    source = select_source_file()
    if not source:
        print("[!] No file selected")
        return
    
    output = select_output_path(source)
    if not output:
        print("[!] No output selected")
        return
    
    compile_file(source, output)
    input("\nPress Enter to exit...")

if __name__ == "__main__":
    main()