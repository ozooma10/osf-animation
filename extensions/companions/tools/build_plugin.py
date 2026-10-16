"""Build the four-record, vanilla-only Sarah dialogue prototype.

Record layouts checked against TES5Edit Core/wbDefinitionsSF1.pas (dev-4.1.6).
No vanilla records are overridden; all added forms live in this plugin.
The top-level player dialogue follows Starfield's SCEN -> DIAL -> INFO model.
"""
from pathlib import Path
import argparse
import struct as s

ROOT = Path(__file__).resolve().parents[1]
QUEST, SCENE, TOPIC, INFO = (0x01000800 + i for i in range(4))


def u32(n): return s.pack('<I', n)
def z(text): return text.encode('utf-8') + b'\0'
def w(text):
    value = text.encode('utf-8')
    return s.pack('<H', len(value)) + value
def field(tag, value=b''): return s.pack('<4sH', tag.encode(), len(value)) + value
def record(tag, form, fields, flags=0):
    data = b''.join(field(*f) for f in fields)
    return s.pack('<4sIIIIHH', tag.encode(), len(data), flags, form, 0, 582, 0) + data
def group(label, data, kind=0):
    return s.pack('<4sI4sIHHHH', b'GRUP', len(data)+24, label, kind, 0, 0, 0, 0) + data
def script(name): return s.pack('<HHH', 6, 2, 1) + w(name) + b'\0\0\0'
def condition(function, value=1., parameter=0, reference=0):
    return s.pack('<IfIIIIIi', 0, value, function, parameter, 0, 2 if reference else 0, reference, -1)


def build():
    fragment = 'Fragments:TopicInfos:TIF_OSFCompanionsHug'
    vmad = s.pack('<HHHBB', 6, 2, 0, 3, 2) + w(fragment) + b'\0\0\0\x01' + w(fragment) + w('Fragment_End')
    quest = record('QUST', QUEST, [
        ('EDID', z('OSFCompanionsQuest')), ('VMAD', script('OSFCompanionsManager')),
        ('FULL', z('OSF Companions')), ('DNAM', s.pack('<IB7x', 0x10419, 50)),
        ('NEXT', b''), ('ANAM', u32(1)), ('ALST', u32(0)),
        ('ALID', z('ConversationPartner')), ('FNAM', u32(2)),
        ('ALFG', u32(0)), ('VTCK', u32(0)), ('ALED', b'')])
    # An optional primary alias is filled from the activated conversation actor.
    # Filter to Sarah's persistent reference, and require both actors out of combat.
    conditions = [('CTDA', condition(136, parameter=0x5986)),
                  ('CTDA', condition(289, 0.)), ('CTDA', condition(289, 0., reference=0x14))]
    scene = record('SCEN', SCENE, [
        ('EDID', z('OSFCompanionsSarahHug')), ('FNAM', u32(0x6810)),
        ('HNAM', b''), ('NAM0', z('Ask for a hug')), ('NEXT', b''),
        ('NEXT', b''), ('WNAM', u32(350)), ('HNAM', b''),
        ('ALID', u32(0)), ('LNAM', u32(8)), ('DNAM', u32(0x10A)),
        ('ALID', u32(0xFFFFFFFE)), ('LNAM', u32(8)), ('DNAM', u32(0x10A)),
        ('ANAM', s.pack('<H', 0)), ('NAM0', z('Can I have a hug?')),
        ('ALID', u32(0xFFFFFFFE)), ('INAM', u32(1)), ('FNAM', u32(0)),
        ('SNAM', u32(0)), ('ENAM', u32(0)), ('DATA', u32(TOPIC)),
        ('DMAX', s.pack('<f', 10)), ('DMIN', s.pack('<f', 1)),
        ('HNAM', b''), ('HTID', u32(0)), ('HNAM', b''), ('ANAM', b''),
        ('PNAM', u32(QUEST)), ('INAM', u32(1)), ('VNAM', u32(1)*5),
        *conditions, ('BOLV', b''), ('XNAM', u32(0)), ('SCPI', s.pack('<H', 50))])
    topic = record('DIAL', TOPIC, [
        ('PNAM', s.pack('<f', 50)), ('QNAM', u32(QUEST)),
        ('DATA', b'\0\x02\x11\0'), ('SNAM', b'SCEN'),
        ('TIFL', u32(INFO)), ('TIFC', u32(1))])
    info = record('INFO', INFO, [
        ('VMAD', vmad), ('ENAM', u32(0x1840)),
        ('TRDA', s.pack('<IIf', 0x112004, 0, -1.)),
        ('NAM1', z('Can I have a hug?')), ('NAM2', z('')),
        ('NAM3', z('')), ('NAM4', z('')), ('NAM0', z('')), ('INAM', u32(1))])
    header = record('TES4', 0, [
        ('HEDR', s.pack('<fII', .96, 8, 0x804)), ('CNAM', z('OSF')),
        ('SNAM', z('Sarah hug prototype for OSF Animation')),
        ('MAST', z('Starfield.esm')), ('INCC', u32(0))], flags=1)
    return header + group(b'QUST', quest) + group(b'DIAL', topic + group(u32(TOPIC), info, 7)) + group(b'SCEN', scene)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, default=ROOT/'dist/OSFCompanions.esm')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build())
    print(f'Built {args.output} ({args.output.stat().st_size} bytes; four new forms, no overrides)')
