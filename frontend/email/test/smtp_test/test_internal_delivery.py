import requests
import time
import socket

HTTP_HOST = "http://localhost:8080"
SMTP_HOST = "localhost"
SMTP_PORT = 2500

def smtp_send_mail(sender, recipient, subject, body):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((SMTP_HOST, SMTP_PORT))
    banner = s.recv(1024).decode()
    assert banner.startswith("220"), f"SMTP not ready: {banner.strip()}"

    def send_cmd(cmd):
        s.sendall((cmd + "\r\n").encode())
        response = s.recv(1024).decode()
        return response

    send_cmd("HELO test.com")
    send_cmd(f"MAIL FROM:<{sender}>")
    send_cmd(f"RCPT TO:<{recipient}>")
    send_cmd("DATA")

    # Email headers + body
    lines = [
        f"To: {recipient}",
        f"From: {sender}",
        f"Subject: {subject}",
        "",
        body,
        "."
    ]
    for line in lines:
        s.sendall((line + "\r\n").encode())
    response = s.recv(1024).decode()
    assert response.startswith("250"), f"SMTP DATA not accepted: {response.strip()}"

    send_cmd("QUIT")
    s.close()
    print(f" SMTP email sent from {sender} to {recipient}")

def signup(username, password):
    r = requests.post(f"{HTTP_HOST}/signup", data={"username": username, "password": password})
    assert r.status_code == 200 or r.status_code == 409
    print(f" Signed up {username}")

def login(username, password):
    r = requests.post(f"{HTTP_HOST}/credentials", data={"username": username, "password": password}, allow_redirects=False)
    assert r.status_code == 200
    cookie = r.headers["Set-Cookie"].split("session_id=")[1].split(";")[0]
    return cookie

def check_inbox(cookie, username, subject, label="inbox"):
    url = f"{HTTP_HOST}/inbox?username={username}"
    r = requests.get(url, cookies={"session_id": cookie})
    assert r.status_code == 200
    if subject in r.text:
        print(f" {subject} found in {username}'s {label}")
    else:
        print(f" {subject} NOT found in {username}'s {label}")

def test_smtp_internal_delivery():
    sender = "alice@localhost"
    recipient = "bob@localhost"
    password = "test123"
    subject = f"SMTP Internal {int(time.time())}"
    body = "Testing SMTP -> KV delivery via socket."

    signup("alice", password)
    signup("bob", password)

    # Send mail via raw SMTP
    smtp_send_mail(sender, recipient, subject, body)

    # Login and check inbox/sent
    alice_cookie = login("alice", password)
    bob_cookie = login("bob", password)

    time.sleep(1)

    check_inbox(bob_cookie, "bob", subject, "inbox")
    check_inbox(alice_cookie, "alice", subject, "sent")

if __name__ == "__main__":
    test_smtp_internal_delivery()
