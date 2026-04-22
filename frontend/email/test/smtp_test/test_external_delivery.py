import socket
import time

SMTP_HOST = "localhost"
SMTP_PORT = 2500

def send_smtp_command(sock, command, expect=None, log=True):
    if log:
        print(f">>> {command}")
    sock.sendall((command + "\r\n").encode())
    response = sock.recv(1024).decode()
    if log:
        print(f"<<< {response.strip()}")
    if expect:
        assert response.startswith(expect), f"Expected {expect}, got: {response.strip()}"
    return response

def test_external_smtp_relay():
    sender = "testsender@seas.upenn.edu"
    recipient = "ashayk@seas.upenn.edu" 
    subject = f"SMTP Relay Test {int(time.time())}"
    message_id = f"<test-{int(time.time())}@seas.upenn.edu>"
    body = "test email relayed via custom SMTP server with MX lookup for my CIS 5050 Project"

    print(" Connecting to local SMTP server...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((SMTP_HOST, SMTP_PORT))
    banner = sock.recv(1024).decode()
    assert banner.startswith("220"), f"SMTP not ready: {banner.strip()}"

    send_smtp_command(sock, "HELO seas.upenn.edu", expect="250")
    send_smtp_command(sock, f"MAIL FROM:<{sender}>", expect="250")
    send_smtp_command(sock, f"RCPT TO:<{recipient}>", expect="250")
    send_smtp_command(sock, "DATA", expect="354")

    headers = [
        f"To: {recipient}",
        f"From: {sender}",
        f"Subject: {subject}",
        f"Message-ID: {message_id}",
        f"X-Mailer: PennCloudSMTP",
        f"Date: {time.strftime('%a, %d %b %Y %H:%M:%S %z')}"
    ]
    email_data = "\r\n".join(headers + ["", body, "."])
    sock.sendall((email_data + "\r\n").encode())
    response = sock.recv(1024).decode()
    print("<<<", response.strip())
    assert response.startswith("250"), f"DATA response not accepted: {response.strip()}"

    send_smtp_command(sock, "QUIT", expect="221")
    sock.close()
    print("External relay command flow complete.")

    print("\n Check your inbox at:", recipient)

if __name__ == "__main__":
    test_external_smtp_relay()
