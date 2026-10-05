"""Verify Starfield PEX native signatures against shipped source and registrations.
PEX reader adapted from OSF UI tests/package/verify_binary.py (same project author).
VM binding and OSFTypes struct layout still require fresh in-game acceptance.
"""
import re
import struct
import sys
from pathlib import Path

def require(condition, message):
    if not condition:
        raise ValueError(message)


class NativePex:
    def __init__(self, path):
        self.script = path.stem.lower()
        self.data = path.read_bytes()
        self.pos = 0

    def number(self, fmt):
        value = struct.unpack_from('<' + fmt, self.data, self.pos)[0]
        self.pos += struct.calcsize(fmt)
        return value

    def skip(self, count):
        self.pos += count
        require(self.pos <= len(self.data), 'truncated PEX')

    def text(self):
        size = self.number('H')
        start = self.pos
        self.skip(size)
        return self.data[start:self.pos].decode('utf-8')

    def name(self):
        return self.strings[self.number('H')].lower().replace('#', ':')

    def value(self):
        kind = self.number('B')
        if kind == 0:
            return None
        if kind in (1, 2):
            return self.name()
        if kind == 3:
            return self.number('i')
        if kind == 4:
            return self.number('f')
        if kind == 5:
            return self.number('B')
        raise ValueError(f'unknown PEX value type {kind}')

    def instructions(self, count):
        # Fixed operand counts: Champollion/Pex/Instruction.cpp.
        arity = [0] + [3]*9 + [2]*5 + [3]*5 + [1,2,2,3,2,3,1,3,3,3,2,2,3,3,4,4,3,1,3,3,5,5,3,3,1,3,1,6,0,0,1]
        for _ in range(count):
            opcode = self.number('B')
            require(opcode < len(arity), f'unsupported PEX opcode {opcode}')
            for _ in range(arity[opcode]):
                self.value()
            if opcode in (23, 24, 25, 48, 49, 50):
                args = self.value()
                require(isinstance(args, int) and 0 <= args <= 65535, 'invalid PEX argument count')
                for _ in range(args):
                    self.value()

    def read(self):
        require(self.number('I') == 0xFA57C0DE, 'invalid PEX magic')
        require(self.number('B') == 3, 'unsupported PEX major')
        self.number('B')
        require(self.number('H') == 4, 'PEX is not Starfield')
        self.skip(8)
        self.text(); self.text(); self.text()
        self.strings = [self.text() for _ in range(self.number('H'))]
        if self.number('B'):
            self.skip(8)
            for _ in range(self.number('H')):
                self.skip(7)
                self.skip(2 * self.number('H'))
            for _ in range(self.number('H')):
                self.skip(10)
                self.skip(2 * self.number('H'))
            for _ in range(self.number('H')):
                self.skip(4)
                self.skip(2 * self.number('H'))
        self.skip(3 * self.number('H'))  # user flag definitions
        require(self.number('H') == 1, 'expected one native object')
        require(self.name() == self.script, 'wrong PEX script')
        self.skip(4)  # object size
        self.name(); self.name()  # parent, documentation
        self.skip(5)  # const flag + user flags
        self.name()  # auto state
        for table in ('structs', 'variables', 'guards', 'properties'):
            require(self.number('H') == 0, f'native-only script unexpectedly contains {table}')
        functions = {}
        for _ in range(self.number('H')):
            self.name()
            for _ in range(self.number('H')):
                name, result = self.name(), self.name()
                self.name(); self.skip(4)
                flags = self.number('B')
                parameters = [(self.name(), self.name()) for _ in range(self.number('H'))]
                locals_count = self.number('H')
                self.skip(4 * locals_count)
                instruction_count = self.number('H')
                self.instructions(instruction_count)
                if flags & 2:
                    require(flags == 3 and locals_count == 0 and instruction_count == 0, f'{name}: invalid Global Native body')
                    require(name not in functions, f'duplicate function {name}')
                    functions[name] = (result, [kind for _, kind in parameters])
        require(self.pos == len(self.data), 'unparsed PEX bytes')
        return functions


def verify(root):
    bindings = (root / 'src/Papyrus/OSFScript.cpp').read_text(encoding='utf-8')
    total = 0
    for script in ('OSF', 'OSFAdvanced'):
        source = (root / f'dist/Scripts/Source/{script}.psc').read_text(encoding='utf-8-sig')
        source = re.sub(r';[^\n]*', '', source)
        pattern = r'^\s*(?:([\w:]+(?:\[\])?)\s+)?Function\s+(\w+)\(([^)]*)\)\s+Global\s+Native\s*$'
        declarations = {}
        for match in re.finditer(pattern, source, re.I | re.M):
            result, name, args = match.groups()
            declarations[name.lower()] = ((result or 'none').lower(), [p.strip().split()[0].lower() for p in args.split(',') if p.strip()])
        functions = NativePex(root / f'dist/Scripts/{script}.pex').read()
        require(functions == declarations, f'{script}: compiled signatures differ from source')
        selector = 'SCRIPT_NAME' if script == 'OSF' else 'ADVANCED_SCRIPT_NAME'
        registered = {n.lower() for n in re.findall(r'BindNativeMethod\(' + selector + r',\s*"(\w+)"', bindings)}
        require(registered == set(declarations), f'{script}: registration names differ: {registered ^ set(declarations)}')
        total += len(functions)
    types = root / 'dist/Scripts/OSFTypes.pex'
    require(types.read_bytes()[:4] == bytes.fromhex('DEC057FA'), 'invalid OSFTypes PEX')
    print(f'Papyrus: {total} compiled native signatures match shipped source and registration names')

if __name__ == '__main__':
    try:
        verify(Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
    except (ValueError, OSError, struct.error) as error:
        print(f'Papyrus validation failed: {error}', file=sys.stderr)
        sys.exit(1)
