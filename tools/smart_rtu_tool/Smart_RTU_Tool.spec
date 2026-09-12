# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_submodules

hiddenimports = ['openpyxl']
hiddenimports += collect_submodules('numpy')
hiddenimports += collect_submodules('pandas')

# --- Added for Linux/STM32MP1 target support ---
hiddenimports += ['paramiko', 'cryptography', 'nacl', 'bcrypt']  # SSH/SFTP (paramiko deps)
hiddenimports += ['paho.mqtt.client']                             # MQTT config push
hiddenimports += ['serial', 'serial.tools.list_ports']            # Serial/USB config push
hiddenimports += ['keyring', 'keyring.backends']                  # secure credential storage
# keyring backend is OS-specific; PyInstaller sometimes needs the exact
# backend module spelled out if auto-detection misses it. Uncomment the
# one matching your build OS if `keyring.get_password` returns None
# silently in the frozen exe when it works fine from source:
# hiddenimports += ['keyring.backends.Windows']   # building on Windows
# hiddenimports += ['keyring.backends.macOS']     # building on macOS
# hiddenimports += ['keyring.backends.SecretService']  # building on Linux

a = Analysis(
    ['run.py'],
    pathex=[],
    binaries=[],
    datas=[],
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)
exe = EXE(
    pyz,
    a.scripts,
    [],
    exclude_binaries=True,
    name='Smart_RTU_Tool',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
coll = COLLECT(
    exe,
    a.binaries,
    a.datas,
    strip=False,
    upx=True,
    upx_exclude=[],
    name='Smart_RTU_Tool',
)
