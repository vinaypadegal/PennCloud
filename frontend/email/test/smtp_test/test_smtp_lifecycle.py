import socket
import threading
import subprocess
import time
import os
import signal

def test_connect_and_banner(host='localhost', port=2500):
    print("Test 1: Connect and check banner")
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect((host, port))
        banner = s.recv(1024).decode()
        assert banner.startswith("220"), f"Expected 220 banner, got: {banner.strip()}"
        print("Server connection and banner OK")
        return s
    except Exception as e:
        print("Connection or banner failed:", e)
        return None

def test_quit_command(sock):
    print("Test 2: QUIT command")
    try:
        sock.sendall(b"QUIT\r\n")
        response = sock.recv(1024).decode()
        assert response.startswith("221"), f"Expected 221 response, got: {response.strip()}"
        print("QUIT command successful")
    except Exception as e:
        print("QUIT command failed:", e)
    finally:
        sock.close()

def connect_and_quit(id, host='localhost', port=2500):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((host, port))
        banner = s.recv(1024).decode()
        assert banner.startswith("220"), f"[Client {id}] Invalid banner: {banner.strip()}"
        s.sendall(b"QUIT\r\n")
        response = s.recv(1024).decode()
        assert response.startswith("221"), f"[Client {id}] Invalid QUIT response: {response.strip()}"
        print(f"Client {id}: Connect + QUIT successful")
        s.close()
    except Exception as e:
        print(f"Client {id}: Failed -", e)

def test_concurrent_clients():
    print("Test 3: Concurrent Clients")
    t1 = threading.Thread(target=connect_and_quit, args=(1,))
    t2 = threading.Thread(target=connect_and_quit, args=(2,))
    t1.start()
    t2.start()
    t1.join()
    t2.join()

def test_graceful_sigint_shutdown(server_path='../../smtp_server', port=2599):
    print("Test 4: SIGINT Graceful Shutdown")

    proc = subprocess.Popen([server_path, '-p', str(port), '-v'],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    time.sleep(1.5)  # Wait for server to start

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(('localhost', port))
        banner = s.recv(1024).decode()
        assert banner.startswith("220"), "Banner not received"

        time.sleep(0.5)  # Give it time to stay open before SIGINT

        os.kill(proc.pid, signal.SIGINT)

        shutdown_msg = ""
        for _ in range(3):
            try:
                shutdown_msg = s.recv(1024).decode()
                if shutdown_msg:
                    break
            except socket.timeout:
                pass
            time.sleep(0.3)

        assert "421" in shutdown_msg, f"Expected 421 shutdown, got: {shutdown_msg.strip()}"
        print("SIGINT Shutdown handled gracefully")

    except Exception as e:
        print("SIGINT Test failed:", e)
    finally:
        try:
            s.close()
        except:
            pass
        try:
            proc.wait(timeout=3)
        except:
            proc.kill()



if __name__ == "__main__":
    sock = test_connect_and_banner()
    if sock:
        test_quit_command(sock)
    test_concurrent_clients()
    test_graceful_sigint_shutdown()
