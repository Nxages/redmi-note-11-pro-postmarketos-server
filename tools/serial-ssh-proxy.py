"""Binary stdin/stdout transport for key-authenticated SSH over the phone's USB serial
rescue channel. Use it as an ssh ProxyCommand:

    ssh -o "ProxyCommand=python3 serial-ssh-proxy.py COM5" root@rescue

Needs pyserial (pip install pyserial). On Linux the port is e.g. /dev/ttyACM0."""
import os
import pathlib
import re
import sys
import threading
import time
import serial

def main():
    if os.name=='nt':
        import msvcrt
        msvcrt.setmode(sys.stdin.fileno(),os.O_BINARY)
        msvcrt.setmode(sys.stdout.fileno(),os.O_BINARY)
    port=serial.Serial(sys.argv[1],baudrate=115200,timeout=0.1,write_timeout=10,
                       rtscts=False,dsrdtr=False,xonxoff=False)
    request=b'STARTSSH\n'
    if port.write(request)!=len(request):
        port.close()
        raise RuntimeError('USB SSH request was not fully queued')
    port.flush()
    deadline=time.monotonic()+20
    line=b''
    diagnostic=re.compile(rb'PMLINUXROOT[A-Z0-9]* (?:BOOT_STAGE=[A-Z0-9_]+|BOOT_FAILURE=[A-Z0-9_]+ LAST_STAGE=[A-Z0-9_]+)')
    while time.monotonic()<deadline:
        char=port.read(1)
        if char==b'\n':
            safe_line=line.rstrip(b'\r')
            if safe_line==b'BOOTSTRAP_READY':
                os.write(sys.stderr.fileno(),b'PMLINUXROOT USB_HANDSHAKE=READY\n')
                break
            if diagnostic.fullmatch(safe_line):
                os.write(sys.stderr.fileno(),safe_line+b'\n')
            line=b''
        elif char:
            line=(line+char)[-200:]
    else:
        port.close()
        raise RuntimeError('USB SSH synchronization timed out')
    stop=threading.Event()
    def send():
        try:
            while not stop.is_set():
                data=os.read(sys.stdin.fileno(),32768)
                if not data: break
                port.write(data)
        except (OSError,serial.SerialException):
            pass
        finally:
            stop.set()
    threading.Thread(target=send,daemon=True).start()
    try:
        while not stop.is_set():
            data=port.read(32768)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    except (OSError,serial.SerialException):
        pass
    finally:
        stop.set()
        port.close()

if __name__=='__main__': main()
