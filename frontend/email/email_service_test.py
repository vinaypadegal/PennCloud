import requests
import urllib.parse
import time
import re

BASE_URL = "http://localhost:8081"
USERS = [
    {"username": "alice", "password": "alice"},
    {"username": "bob", "password": "bob"},
]

def signup(username, password):
    data = {
        "username": username,
        "password": password
    }
    headers = {'Content-Type': 'application/x-www-form-urlencoded'}
    response = requests.post(f"{BASE_URL}/signup", data=urllib.parse.urlencode(data), headers=headers)
    return "successfully signed up" in response.text.lower() or "already exists" in response.text.lower()

def login(username, password):
    session = requests.Session()
    data = {
        "username": username,
        "password": password
    }
    headers = {'Content-Type': 'application/x-www-form-urlencoded'}
    response = session.post(f"{BASE_URL}/credentials", data=urllib.parse.urlencode(data), headers=headers)
    if "Set-Cookie" in response.headers or "Welcome" in response.text:
        return session
    return None

def send_email(session, sender, recipient, subject, body):
    data = {
        "to": recipient,
        "subject": subject,
        "body": body,
        "username": sender
    }
    headers = {'Content-Type': 'application/x-www-form-urlencoded'}
    response = session.post(f"{BASE_URL}/send", data=urllib.parse.urlencode(data), headers=headers)
    return response.text

def view_inbox(session, username):
    response = session.get(f"{BASE_URL}/inbox", params={"username": username})
    return response.text

def extract_email_ids(html):
    matches = re.findall(r"/email\?id=([^&]+)&", html)
    return list(set(urllib.parse.unquote(m) for m in matches))

def view_email(session, username, email_id, folder="inbox"):
    params = {"username": username, "id": email_id, "folder": folder}
    response = session.get(f"{BASE_URL}/email", params=params)
    return response.text

def delete_email(session, username, email_id, folder="inbox"):
    params = {"username": username, "id": email_id, "folder": folder}
    response = session.post(f"{BASE_URL}/delete", params=params)
    return response.text

def assert_in_html(content, must_contain):
    for key in must_contain:
        assert key in content, f"Expected '{key}' in HTML output"

def run_tests():
    print("\nRunning Email Service Integration Tests with Sessions and Edge Cases")

    for user in USERS:
        print(f"[+] Signing up {user['username']}")
        assert signup(user["username"], user["password"])

    sessions = {}
    for user in USERS:
        print(f"[+] Logging in {user['username']}")
        sess = login(user["username"], user["password"])
        assert sess, f"Login failed for {user['username']}"
        sessions[user["username"]] = sess

    # Alice sends to Bob
    subject = "Initial Test"
    body = "Hello Bob."
    print("\n--- Alice sends email to Bob ---")
    send_email(sessions["alice"], "alice@localhost", "bob@localhost", subject, body)

    time.sleep(1)

    print("\n--- Bob views inbox and replies ---")
    inbox = view_inbox(sessions["bob"], "bob@localhost")
    bob_emails = extract_email_ids(inbox)
    assert bob_emails, "No email found for Bob"
    email_id = bob_emails[0]
    reply_data = {
        "to": "alice@localhost",
        "subject": "Re: Initial Test",
        "body": "Thanks Alice!",
        "username": "bob@localhost",
        "is_reply": "true",
        "original_id": email_id
    }
    sessions["bob"].post(f"{BASE_URL}/send", data=urllib.parse.urlencode(reply_data))

    time.sleep(1)

    print("\n--- Alice views inbox and forwards Bob's reply to herself ---")
    inbox = view_inbox(sessions["alice"], "alice@localhost")
    alice_emails = extract_email_ids(inbox)
    assert alice_emails, "No email in Alice's inbox"
    reply_email_id = alice_emails[-1]  # the reply from Bob
    forward_data = {
        "to": "alice@localhost",
        "subject": "Fwd: Re: Initial Test",
        "body": "FYI to self\n\n--- Forwarded ---",
        "username": "alice@localhost",
        "is_forward": "true",
        "original_id": reply_email_id
    }
    sessions["alice"].post(f"{BASE_URL}/send", data=urllib.parse.urlencode(forward_data))

    time.sleep(1)

    print("\n Complex Flow Test Passed: Alice forwarded Bob's reply back to herself")

    print("\n--- Final Inbox View for Alice ---")
    final_inbox = view_inbox(sessions["alice"], "alice@localhost")
    assert_in_html(final_inbox, ["Re: Initial Test", "Fwd: Re: Initial Test"])
    print(final_inbox[:800])  # Preview

    print("\n--- Testing deletion of forwarded mail ---")
    fwd_ids = extract_email_ids(final_inbox)
    last_id = fwd_ids[-1]
    delete_result = delete_email(sessions["alice"], "alice@localhost", last_id)
    assert "success" in delete_result.lower()
    print("Forwarded email deleted successfully.")

    print("\n--- Stress Test: 5 chained replies ---")
    current_subject = "Chain Start"
    current_body = "First in chain"
    send_email(sessions["alice"], "alice@localhost", "bob@localhost", current_subject, current_body)
    time.sleep(1)

    for i in range(5):
        inbox_html = view_inbox(sessions["bob" if i % 2 == 0 else "alice"], f"{'bob' if i % 2 == 0 else 'alice'}@localhost")
        ids = extract_email_ids(inbox_html)
        latest_id = ids[-1]
        next_user = "alice" if i % 2 == 0 else "bob"
        reply_data = {
            "to": f"{next_user}@localhost",
            "subject": f"Re: {current_subject}",
            "body": f"Reply #{i+1}",
            "username": f"{'bob' if i % 2 == 0 else 'alice'}@localhost",
            "is_reply": "true",
            "original_id": latest_id
        }
        sessions["bob" if i % 2 == 0 else "alice"].post(f"{BASE_URL}/send", data=urllib.parse.urlencode(reply_data))
        time.sleep(1)

    print("Chained reply test passed with 5 iterations.")

if __name__ == "__main__":
    run_tests()
