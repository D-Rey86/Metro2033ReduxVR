"""Check adapter hook signatures against an archived mapped Metro image."""
import argparse
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('image', type=Path)
args = parser.parse_args()
data = args.image.read_bytes()
source = (Path(__file__).resolve().parents[1] /
          'ThirdParty/3Dmigoto/DirectX11/NativeCameraCreditAdapter.inl').read_text()
for name, rva in [('b', 0xD560), ('c', 0xD5350), ('r', 0xD6BE0),
                  ('e', 0x7EE2E0), ('s', 0x7E44D0)]:
    body = re.search(r'static const BYTE ' + name + r'\[\] = \{([^}]+)\}', source)[1]
    expected = bytes(int(x, 16) for x in body.split(','))
    if data[rva:rva+len(expected)] != expected:
        raise SystemExit(f'FAIL: signature {name} at {rva:#x}')
    print(f'PASS: {name} at {rva:#x}, {len(expected)} bytes')
