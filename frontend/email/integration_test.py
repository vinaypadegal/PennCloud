import smtplib
import requests
import urllib.parse
import time
import re
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart

# BASES
HTTP_BASE = "http://localhost:8081"
SMTP_HOST = "localhost"
SMTP_PORT = 2500

USERS = [
    {"username": "alice", "password": "alice"},
    {"username": "bob", "password": "bob"},
    {"username": "ashaykatre", "password": "ashaykatre"}
]

def signup_and_login():
    sessions = {}
    for user in USERS:
        # Signup
        requests.post(f"{HTTP_BASE}/signup", data=urllib.parse.urlencode(user), headers={'Content-Type': 'application/x-www-form-urlencoded'})
        # Login
        session = requests.Session()
        res = session.post(f"{HTTP_BASE}/credentials", data=urllib.parse.urlencode(user), headers={'Content-Type': 'application/x-www-form-urlencoded'})
        assert "Set-Cookie" in res.headers or "Welcome" in res.text
        sessions[user["username"]] = session
    return sessions

def send_internal(session, sender, recipient, subject, body):
    data = {
        "to": recipient,
        "subject": subject,
        "body": body,
        "username": sender
    }
    headers = {'Content-Type': 'application/x-www-form-urlencoded'}
    r = session.post(f"{HTTP_BASE}/send", data=urllib.parse.urlencode(data), headers=headers)
    return r.status_code == 200

def extract_email_ids(html):
    matches = re.findall(r"/email\?id=([^&]+)&", html)
    return list(set(urllib.parse.unquote(m) for m in matches))

def view_inbox(session, username):
    res = session.get(f"{HTTP_BASE}/inbox", params={"username": username})
    assert res.status_code == 200
    return res.text

def send_via_smtp(sender, recipient, subject, body):
    try:
        msg = MIMEMultipart()
        msg['From'] = sender
        msg['To'] = recipient
        msg['Subject'] = subject
        msg.attach(MIMEText(body, 'plain'))

        with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as server:
            server.sendmail(sender, recipient, msg.as_string())
        return True
    except Exception as e:
        print(f"SMTP send failed: {e}")
        return False

def run_tests():
    print("\n Starting PennCloud Intermediate Integration Test")

    sessions = signup_and_login()

    # Step 1: Internal send from Alice to Bob
    print("\n[1] Alice sends an internal email to Bob")
    assert send_internal(sessions["alice"], "alice@localhost", "bob@localhost", "Hello Internal", "Hi Bob via PennCloud!")
    time.sleep(1)

    inbox_html = view_inbox(sessions["bob"], "bob@localhost")
    assert "Hello Internal" in inbox_html
    print(" Internal email appears in Bob's inbox")

    # Step 2: SMTP send from Thunderbird/emulated client to Bob
    print("\n[2] External SMTP email to Bob via SMTP Server")
    assert send_via_smtp("external@localhost", "bob@localhost", "Hello from Thunderbird", "SMTP test message")
    time.sleep(1)

    inbox_html = view_inbox(sessions["bob"], "bob@localhost")
    assert "Hello from Thunderbird" in inbox_html
    print(" SMTP-based email appears in Bob's inbox")

    # Step 3: Thunderbird/emulated SMTP client sends to external domain (manually verify)
    print("\n[3] [OPTIONAL] SMTP to external domain (manual verification if DNS + relay is set)")
    print("If you configured outbound DNS + MX relay, test it with: \n  external@localhost -> you@gmail.com or you@seas.upenn.edu")
    print("(This is not enforced in intermediate test)")

    # Step 4: Forward from Bob to Alice
    print("\n[4] Bob forwards the SMTP message to Alice")
    email_ids = extract_email_ids(inbox_html)
    forward_data = {
        "to": "alice@localhost",
        "subject": "Fwd: Hello from Thunderbird",
        "body": "FYI forwarded",
        "username": "bob@localhost",
        "is_forward": "true",
        "original_id": email_ids[-1]
    }
    r = sessions["bob"].post(f"{HTTP_BASE}/send", data=urllib.parse.urlencode(forward_data), headers={'Content-Type': 'application/x-www-form-urlencoded'})
    assert r.status_code == 200
    time.sleep(1)

    inbox_html = view_inbox(sessions["alice"], "alice@localhost")
    assert "Fwd: Hello from Thunderbird" in inbox_html
    print(" Forwarded message appears in Alice's inbox")

    print("\n[5] SMTP send to ashaykatre@localhost (Thunderbird user)")

    assert send_via_smtp("external@localhost", "ashaykatre@localhost", "Test for Ashay", "From external client to ashaykatre")
    time.sleep(1)

    inbox_html = view_inbox(sessions["ashaykatre"], "ashaykatre@localhost")
    assert "Test for Ashay" in inbox_html
    print(" SMTP-based email appears in ashaykatre@localhost's inbox")


    print("\n All intermediate functionality is working correctly")

if __name__ == "__main__":
    run_tests()
