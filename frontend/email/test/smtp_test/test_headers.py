import socket
import time
import requests
import re

SMTP_HOST = "localhost"
SMTP_PORT = 2500
HTTP_HOST = "http://localhost:8080"

def send_smtp_bare_email(sender, recipient, body):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((SMTP_HOST, SMTP_PORT))
    banner = s.recv(1024).decode()

    def cmd(command): s.sendall((command + "\r\n").encode()); return s.recv(1024).decode()

    cmd("HELO test.com")
    cmd(f"MAIL FROM:<{sender}>")
    cmd(f"RCPT TO:<{recipient}>")
    cmd("DATA")

    # Send NO headers — just a blank line + body
    s.sendall(b"\r\n")  # <-- end of headers
    s.sendall((body + "\r\n").encode())
    s.sendall(b".\r\n")
    s.recv(1024)
    cmd("QUIT")
    s.close()
    print("Email sent with no headers (bare DATA)")

def login_and_get_inbox(username, password):
    r = requests.post(f"{HTTP_HOST}/credentials", data={"username": username, "password": password}, allow_redirects=False)
    cookie = r.headers["Set-Cookie"].split("session_id=")[1].split(";")[0]
    r = requests.get(f"{HTTP_HOST}/inbox?username={username}", cookies={"session_id": cookie})
    return r.text

def test_header_autofill():
    sender = "alice@localhost"
    recipient = "bob@localhost"
    password = "test123"
    body = f"This is a body-only message sent at {time.strftime('%H:%M:%S')}."

    send_smtp_bare_email(sender, recipient, body)
    time.sleep(1)

    html = login_and_get_inbox("bob", password)
    print("Inbox HTML fetched")

    checks = {
        "From header present": "alice@localhost" in html,
        "To header present": "bob@localhost" in html,
        "Message-ID present": bool(re.search(r"<.*@.*>", html)),
        "Date header present": bool(re.search(r"\d{4}-\d{2}-\d{2}", html)),
        "Subject defaulted": "No Subject" in html
    }

    for label, passed in checks.items():
        print(f"{'Works' if passed else 'Error'} {label}")

if __name__ == "__main__":
    test_header_autofill()
