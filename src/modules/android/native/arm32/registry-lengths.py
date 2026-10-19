"""Read original Khronos command length contracts without parsing C declarations."""
import json
import pathlib
import sys
import xml.etree.ElementTree as ET

commands = {}
aliases = {}
registry = ET.parse(sys.argv[1]).getroot()
if len(sys.argv) > 2:
    pathlib.Path(sys.argv[2]).write_text(registry.findtext('comment').strip() + '\n')
for command in registry.findall('./commands/command'):
    name = command.findtext('proto/name')
    if name:
        commands[name] = [dict(name=parameter.findtext('name'), length=parameter.get('len'))
                          for parameter in command.findall('param')]
        if command.find('alias') is not None:
            aliases[name] = command.find('alias').get('name')
# Registry extension aliases sometimes retain a canonical parameter name in
# their length expression while renaming that parameter in the declaration.
# Original alias relationships supply the ordinal correspondence.
for name, target in aliases.items():
    if target not in commands or len(commands[name]) != len(commands[target]):
        continue
    for parameter, canonical in zip(commands[name], commands[target]):
        parameter['aliases'] = [canonical['name']]
print(json.dumps(commands, separators=(',', ':'), sort_keys=True))
