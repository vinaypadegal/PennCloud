import os
import subprocess

def kill_processes_by_name(name):
    """Kill all processes matching a given name."""
    try:
        output = subprocess.check_output(f"pgrep -f '{name}'", shell=True).decode().split()
        for pid in output:
            print(f"Killing {name} with PID {pid}")
            subprocess.call(f"kill -9 {pid}", shell=True)
    except subprocess.CalledProcessError:
        print(f"No active process found for: {name}")

if __name__ == "__main__":
    print("Shutting down coordinator, workers, and HTTP servers...\n")

    kill_processes_by_name("./coordinator")
    kill_processes_by_name("./worker")
    kill_processes_by_name("./http_server")
    kill_processes_by_name("./load_balancer")
    kill_processes_by_name("./smtp_server")

    print("\nAll known processes have been terminated.")
