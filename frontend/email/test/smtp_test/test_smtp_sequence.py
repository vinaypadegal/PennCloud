import socket

def connect_to_server(host='localhost', port=2500):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    s.connect((host, port))
    banner = s.recv(1024).decode()
    assert banner.startswith("220"), f"Expected 220 banner, got: {banner.strip()}"
    return s

def send_and_expect(sock, command, expected_start, description):
    print(f"Test: {description}")
    sock.sendall((command + "\r\n").encode())
    response = sock.recv(1024).decode()
    if expected_start:
        assert response.startswith(expected_start), f"{command} → Expected '{expected_start}', got: {response.strip()}"
        print(f"{command} → {response.strip()}")
    else:
        print(f"{command} → {response.strip()}")
    return response

def test_sequence_validations():
    print("==== SMTP Command Sequence Validation ====")

    # Test 1: RCPT TO before MAIL FROM
    s1 = connect_to_server()
    send_and_expect(s1, "HELO test.com", "250", "HELO first")
    send_and_expect(s1, "RCPT TO:<bob@localhost>", "503", "RCPT TO before MAIL FROM (should fail)")
    send_and_expect(s1, "QUIT", "221", "Close")
    s1.close()

    # Test 2: DATA before RCPT TO
    s2 = connect_to_server()
    send_and_expect(s2, "HELO test.com", "250", "HELO again")
    send_and_expect(s2, "MAIL FROM:<alice@localhost>", "250", "MAIL FROM valid")
    send_and_expect(s2, "DATA", "503", "DATA before RCPT TO (should fail)")
    send_and_expect(s2, "QUIT", "221", "Close")
    s2.close()

    # Test 3: DATA without MAIL FROM
    s3 = connect_to_server()
    send_and_expect(s3, "HELO test.com", "250", "HELO again")
    send_and_expect(s3, "DATA", "503", "DATA without MAIL FROM (should fail)")
    send_and_expect(s3, "QUIT", "221", "Close")
    s3.close()

    # Test 4: Multiple RCPT TO + DATA blocks
    s4 = connect_to_server()
    send_and_expect(s4, "HELO test.com", "250", "HELO")
    send_and_expect(s4, "MAIL FROM:<alice@localhost>", "250", "MAIL FROM")
    send_and_expect(s4, "RCPT TO:<bob@localhost>", "250", "First RCPT TO")
    send_and_expect(s4, "DATA", "354", "First DATA block")
    s4.sendall(b"Subject: First Message\r\n\r\nThis is first.\r\n.\r\n")
    assert s4.recv(1024).decode().startswith("250")

    send_and_expect(s4, "RCPT TO:<carol@localhost>", "250", "Second RCPT TO after DATA")
    send_and_expect(s4, "DATA", "354", "Second DATA block")
    s4.sendall(b"Subject: Second Message\r\n\r\nThis is second.\r\n.\r\n")
    assert s4.recv(1024).decode().startswith("250")
    
    send_and_expect(s4, "QUIT", "221", "Close")
    s4.close()

    # Test 5: MAIL FROM resets recipients
    s5 = connect_to_server()
    send_and_expect(s5, "HELO test.com", "250", "HELO")
    send_and_expect(s5, "MAIL FROM:<alice@localhost>", "250", "First MAIL FROM")
    send_and_expect(s5, "RCPT TO:<bob@localhost>", "250", "First RCPT TO")
    send_and_expect(s5, "MAIL FROM:<carol@localhost>", "250", "Second MAIL FROM resets recipients")
    send_and_expect(s5, "DATA", "503", "DATA without new RCPT TO (old one should be cleared)")
    send_and_expect(s5, "QUIT", "221", "Close")
    s5.close()

if __name__ == "__main__":
    test_sequence_validations()
