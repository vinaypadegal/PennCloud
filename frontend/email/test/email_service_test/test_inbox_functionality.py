import socket
import time
import requests
import re

SMTP_HOST = "localhost"
SMTP_PORT = 2500
HTTP_HOST = "http://localhost:8080"

def signup(username, password):
    r = requests.post(f"{HTTP_HOST}/signup", data={"username": username, "password": password})
    if r.status_code == 409:
        print(f"{username} already signed up.")
    elif r.status_code == 200:
        print(f"Signed up {username}")
    else:
        raise Exception(f"Failed to signup {username}: {r.status_code}")

def login_get_cookie(username, password):
    r = requests.post(f"{HTTP_HOST}/credentials", data={"username": username, "password": password}, allow_redirects=False)
    assert r.status_code == 200, f"Login failed for {username}: {r.status_code}"
    cookie = r.headers["Set-Cookie"].split("session_id=")[1].split(";")[0]
    print(f"Logged in as {username}")
    return cookie

def send_email_smtp(sender, recipient, subject, body):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect((SMTP_HOST, SMTP_PORT))
    banner = s.recv(1024).decode()

    def send_cmd(cmd): s.sendall((cmd + "\r\n").encode()); return s.recv(1024).decode()

    send_cmd("HELO localhost")
    send_cmd(f"MAIL FROM:<{sender}>")
    send_cmd(f"RCPT TO:<{recipient}>")
    send_cmd("DATA")

    s.sendall(f"Subject: {subject}\r\n".encode())
    s.sendall(f"From: {sender}\r\n".encode())
    s.sendall(f"To: {recipient}\r\n".encode())
    s.sendall(b"\r\n")  
    s.sendall((body + "\r\n").encode())
    s.sendall(b".\r\n")

    s.recv(1024)
    send_cmd("QUIT")
    s.close()
    print(f"Email sent from {sender} to {recipient}")

def extract_email_id(html, subject):
    pattern = rf"<td>{re.escape(subject)}</td>"
    if re.search(pattern, html):
        rows = html.split("<tr>")
        for row in rows:
            if subject in row:
                match = re.search(r"<td>([^<]+)</td>", row)
                if match:
                    return match.group(1)
    return None

def test_inbox():
    sender_user = "alice"
    recipient_user = "bob"
    sender_email = sender_user + "@localhost"
    recipient_email = recipient_user + "@localhost"
    password = "test123"
    subject = f"Inbox Test {int(time.time())}"
    body = "This is a test email to check inbox functionality."

    # 1. Sign up both users
    signup(sender_user, password)
    signup(recipient_user, password)

    # 2. Send email via SMTP
    send_email_smtp(sender_email, recipient_email, subject, body)
    time.sleep(1)

    # 3. Log in as recipient
    cookie = login_get_cookie(recipient_user, password)

    # 4. Fetch inbox
    r = requests.get(f"{HTTP_HOST}/inbox?username={recipient_email}", cookies={"session_id": cookie})
    assert r.status_code == 200
    html = r.text
    print("Inbox fetched")

    assert subject in html, "Subject not found in inbox"
    assert sender_email in html, "Sender not listed"
    assert re.search(r"\d{4}-\d{2}-\d{2}", html), "Timestamp not found"
    print("Subject, sender, and date present")

    # 5. Extract email ID and view
    email_id = extract_email_id(html, subject)
    assert email_id, "Email ID could not be extracted from inbox"

    view_url = f"{HTTP_HOST}/email?id={email_id}&folder=inbox&username={recipient_email}"
    r = requests.get(view_url, cookies={"session_id": cookie})
    assert r.status_code == 200
    assert subject in r.text, "Subject not found in email view"
    assert body in r.text, "Body not found in email view"
    print("Email view content verified")

if __name__ == "__main__":
    test_inbox()
