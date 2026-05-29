import paramiko

client = paramiko.SSHClient()
client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
client.connect('192.168.0.100', username='admin', password='password', timeout=10)

def run(cmd):
    _, stdout, stderr = client.exec_command(cmd)
    return stdout.read().decode()

print('=== protonodes.xml (Modbus register map) ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/workspace/module_1/config/protonodes.xml'))

print('=== tags.xml (first 80 lines) ===')
print(run('head -80 /mnt/data/hmi/qthmi/deploy/workspace/module_1/config/tags.xml'))

print('=== protocolfilelist.xml ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/protocols/protocolfilelist.xml'))

print('=== CODESYSControl.cfg ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/rts/CODESYSControl.cfg'))

print('=== engineconfig.xml ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/workspace/module_1/config/engineconfig.xml'))

print('=== start.sh (main HMI start script) ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/start.sh'))

print('=== xplc/start.sh ===')
print(run('cat /mnt/data/hmi/qthmi/deploy/xplc/start.sh'))

client.close()
