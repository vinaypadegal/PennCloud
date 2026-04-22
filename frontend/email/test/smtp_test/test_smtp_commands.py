import socket

def send_and_expect(sock, command, expected_start, description):
    print(f"Test: {description}")
    try:
        sock.sendall((command + "\r\n").encode())
        response = sock.recv(1024).decode()
        assert response.startswith(expected_start), f"Expected '{expected_start}', got: {response.strip()}"
        print(f" {command.strip()} → {response.strip()}")
    except Exception as e:
        print(f" {command.strip()} failed:", e)

def test_smtp_commands(host='localhost', port=2500):
    print("==== SMTP Command Syntax Tests ====")

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5)
        sock.connect((host, port))

        banner = sock.recv(1024).decode()
        assert banner.startswith("220"), f"Expected 220 banner, got: {banner.strip()}"
        print("Connection banner received")

        send_and_expect(sock, "HELO test.com", "250", "HELO domain")
        send_and_expect(sock, "MAIL FROM:<alice@localhost>", "250", "MAIL FROM valid")
        send_and_expect(sock, "RCPT TO:<bob@localhost>", "250", "RCPT TO valid")

        send_and_expect(sock, "DATA", "354", "DATA start")
        sock.sendall(b"Subject: Test Email\r\n\r\nHello World\r\n.\r\n")
        response = sock.recv(1024).decode()
        assert response.startswith("250"), f"End of DATA failed: {response.strip()}"
        print("End of DATA with '.' →", response.strip())

        send_and_expect(sock, "NOOP", "250", "NOOP")
        send_and_expect(sock, "RSET", "250", "RSET clears state")

        send_and_expect(sock, "QUIT", "221", "QUIT session")

    except Exception as e:
        print("Test setup failed:", e)
    finally:
        sock.close()

if __name__ == "__main__":
    test_smtp_commands()
