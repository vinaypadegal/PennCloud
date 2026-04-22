import smtplib
from email.message import EmailMessage
import time

SMTP_HOST = 'localhost'
SMTP_PORT = 2500

# Utility to pause and separate output
def pause(msg="..."):
    print(f"\n{'='*40}\n{msg}\n{'='*40}\n")
    time.sleep(1)

def test_send_email(sender, recipients, subject=None, body=None):
    msg = EmailMessage()
    msg['From'] = sender
    msg['To'] = ', '.join(recipients)
    if subject:
        msg['Subject'] = subject
    else:
        msg['Subject'] = ''  # simulate missing subject

    msg.set_content(body or "This is a test body.")

    try:
        with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as server:
            server.set_debuglevel(1)  # print SMTP conversation
            server.send_message(msg)
            print(f"Sent from {sender} to {recipients}")
    except Exception as e:
        print(f"Failed to send: {e}")

def test_all():
    pause("Test 1: Local sender to local recipient (with subject)")
    test_send_email(
        sender="alice@localhost",
        recipients=["bob@localhost"],
        subject="Local to Local Email",
        body="Hello Bob, this is Alice."
    )

    pause("Test 2: External sender to local recipient (should store only in recipient inbox)")
    test_send_email(
        sender="external@example.com",
        recipients=["bob@localhost"],
        subject="External Test Email",
        body="External test to Bob"
    )

    pause("Test 3: Local to multiple local recipients")
    test_send_email(
        sender="carol@localhost",
        recipients=["bob@localhost", "dave@localhost"],
        subject="Group Email",
        body="Hi team, please find updates."
    )

    pause("Test 4: Local sender with NO subject")
    test_send_email(
        sender="eve@localhost",
        recipients=["bob@localhost"],
        body="This has no subject, should be stored with default"
    )

    pause("Test 5: Simulated malformed or raw body (no headers)")
    # Directly connect like telnet
    try:
        with smtplib.SMTP(SMTP_HOST, SMTP_PORT) as server:
            server.set_debuglevel(1)
            server.helo("gmail.com")
            server.mail("rawsender@localhost")
            server.rcpt("bob@localhost")
            server.data()
            server.send("This is just a raw line without subject\r\nAnother line\r\n.\r\n")
            print("Raw email sent")
    except Exception as e:
        print(f"Raw test failed: {e}")

if __name__ == "__main__":
    test_all()
