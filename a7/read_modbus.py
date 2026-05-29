import paramiko

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('192.168.0.100', username='admin', password='password', timeout=10)

def run(cmd):
    _, stdout, stderr = client.exec_command(cmd)
    return stdout.read().decode()

print('=== /mnt/data/hmi/qthmi/deploy/xplc/ ===')
print(run('ls -la /mnt/data/hmi/qthmi/deploy/xplc/'))

print('=== LLExecLinux.conf ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/xplc/LLExecLinux.conf'))

print('=== protocols.xml ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/workspace/module_1/config/protocols.xml'))

print('=== /mnt/data/hmi/qthmi/deploy/protocols/ ===')
print(run('ls -la /mnt/data/hmi/qthmi/deploy/protocols/'))

print('=== rts/3S_INFO.txt ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/rts/3S_INFO.txt'))

print('=== hmiversion.xml (first 40 lines) ===')
print(run('head -40 /mnt/data/hmi/qthmi/deploy/hmiversion.xml'))

print('=== workspace structure ===')
print(run('find /mnt/data/hmi/qthmi/deploy/workspace -type f 2>/dev/null'))

print('=== rts directory ===')
print(run('ls -la /mnt/data/hmi/qthmi/deploy/rts/'))

print('=== strings from LLModbusTCP.so (first 60 lines of strings) ===')
print(run('strings /mnt/data/hmi/qthmi/deploy/xplc/LLModbusTCP.so 2>/dev/null | grep -i -E "modbus|port|tcp|connect|register|coil|read|write|slave|master|function" | head -60'))

print('=== strings from LLModbusRTU.so ===')
print(run('strings /mnt/data/hmi/qthmi/deploy/xplc/LLModbusRTU.so 2>/dev/null | grep -i -E "modbus|rtu|serial|baud|port|register|coil|read|write" | head -60'))

client.close()
