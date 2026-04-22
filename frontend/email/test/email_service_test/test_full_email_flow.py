import requests
import time

BASE_URL = "http://localhost:8081"

def signup(username, password):
    print(f"Signing up: {username}")
    r = requests.post(f"{BASE_URL}/signup", data={"username": username, "password": password})
    assert r.status_code == 200

def login(username, password):
    print(f"Logging in: {username}")
    r = requests.post(f"{BASE_URL}/credentials", data={"username": username, "password": password}, allow_redirects=False)
    assert r.status_code == 200
    return r.cookies["session_id"]

def send_email(cookie, sender, to, subject, body):
    print(f"{sender} → {to} | {subject}")
    r = requests.post(f"{BASE_URL}/send", data={
        "username": sender,
        "to": to,
        "subject": subject,
        "body": body
    }, cookies={"session_id": cookie})
    assert r.status_code == 200

def get_inbox(cookie, username):
    print(f"Fetching inbox for: {username}")
    r = requests.get(f"{BASE_URL}/inbox?username={username}", cookies={"session_id": cookie})
    assert r.status_code == 200
    return r.text

def reply_email(cookie, username, email_id, folder, body):
    print(f"↩Replying from {username} to email ID: {email_id}")
    r = requests.post(f"{BASE_URL}/send", data={
        "username": username,
        "is_reply": "true",
        "original_id": email_id,
        "to": "",  # populated automatically
        "subject": "",  # auto-generated
        "body": body
    }, cookies={"session_id": cookie})
    assert r.status_code == 200

def forward_email(cookie, username, email_id, subject, body, to):
    print(f"Forwarding from {username} email ID: {email_id} → {to}")
    r = requests.post(f"{BASE_URL}/send", data={
        "username": username,
        "is_forward": "true",
        "original_id": email_id,
        "to": to,
        "subject": subject,
        "body": body
    }, cookies={"session_id": cookie})
    assert r.status_code == 200

def delete_email(cookie, username, email_id, folder):
    print(f"Deleting email ID: {email_id} from {username}'s {folder}")
    r = requests.post(f"{BASE_URL}/delete?id={email_id}&folder={folder}&username={username}",
                      cookies={"session_id": cookie})
    assert r.status_code == 200

def extract_latest_email_id(html):
    start = html.find("email?id=")
    if start == -1:
        return None
    start += len("email?id=")
    end = html.find("&", start)
    return html[start:end]

def run_email_test():
    users = [("alice", "alice"), ("bob", "bob"), ("chris", "chris")]
    sessions = {}

    # Step 1: Sign up and login
    for username, password in users:
        signup(username, password)
        cookie = login(username, password)
        sessions[username] = cookie

    # Step 2: alice → bob
    send_email(sessions["alice"], "alice@localhost", "bob@localhost", "Test 1", "Send to Bob - Test 1")
    time.sleep(1)
    inbox_bob = get_inbox(sessions["bob"], "bob@localhost")
    email_id = extract_latest_email_id(inbox_bob)
    assert email_id

    # Step 3: bob replies to alice
    reply_body = "Hi Alice, got this from you. Seems to work fine"
    reply_email(sessions["bob"], "bob@localhost", email_id, "inbox", reply_body)
    time.sleep(1)
    inbox_alice = get_inbox(sessions["alice"], "alice@localhost")
    reply_id = extract_latest_email_id(inbox_alice)
    assert reply_id

    # Step 4: alice forwards to chris
    forward_body = "Hey Chris, I sent this to Bob\n" + reply_body
    forward_email(sessions["alice"], "alice@localhost", reply_id, "Fwd: Test 1", forward_body, "chris@localhost")
    time.sleep(1)
    inbox_chris = get_inbox(sessions["chris"], "chris@localhost")
    chris_id = extract_latest_email_id(inbox_chris)
    assert chris_id

    # Step 5: chris deletes
    delete_email(sessions["chris"], "chris@localhost", chris_id, "inbox")

    # Step 6: Bob deletes the original email from Alice
    delete_email(sessions["bob"], "bob@localhost", email_id, "inbox")
    print("Bob deleted Alice's original email")

    # Step 7: Alice forwards Bob's reply to Chris again
    forward_email(
        sessions["alice"],
        "alice@localhost",
        reply_id,
        "Fwd again: Re: Test 1",
        "Hey Chris, here's Bob's reply again.",
        "chris@localhost"
    )
    print("Alice forwarded Bob's reply to Chris again")

    # Step 8: Bob sends a very large email to Chris (5000+ characters)
    large_body = "This is a large test message.\n" + ("Lorem ipsum " * 500)
    send_email(sessions["bob"], "bob@localhost", "chris@localhost", "Large Test", large_body)
    print("Bob sent a large email to Chris")


    print("\nAll tests passed: signup, send, reply, forward, delete")

if __name__ == "__main__":
    run_email_test()
