import os
import subprocess
import time

os.chdir(os.path.join(os.path.dirname(__file__), "../../../build"))


def launch_background_process(cmd, log_file):
    """Launch a process in the background and redirect output to a log file."""
    os.makedirs("logs", exist_ok=True)
    with open(log_file, 'w') as log:
        process = subprocess.Popen(cmd, stdout=log, stderr=log, shell=True)
    print(f"Launched: {cmd} → {log_file}")
    return process

# Step 1: Launch coordinator
launch_background_process("./coordinator", "../backend/logs/coordinator.log")
time.sleep(0.1)


# Step 2: Launch 6 workers
for i in range(1, 7):
    launch_background_process(f"./worker {i}", f"../backend/logs/worker{i}.txt")
    time.sleep(0.1)

# Step 3: Launch 3 frontend HTTP servers
http_ports = [8081, 8082, 8083]
for i, port in enumerate(http_ports):
    launch_background_process(f"./http_server -v -p {port}", f"logs/http_server{i+1}.log")

launch_background_process(f"./load_balancer -p 8080", "logs/load_balancer.log")

launch_background_process(f"./smtp_server -p 2500 -v -x", "logs/smtp_server.log")


# Step 4: Wait a few seconds to let all servers initialize
print("Waiting for servers to come up...")
time.sleep(5)

print("\" All components launched successfully.")
print(" Logs are saved in ./logs/")
print(" You can now run email tests, or browse to http://localhost:8080")
