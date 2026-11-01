"""Deterministic, synthetic still-image corpus for native GPU qualification.
No service operations, network access, credentials or model assets at import.
"""
import base64
import json
import struct
import zlib


def png(rgb, w=256, h=256):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress((b'\0' + bytes(rgb) * w) * h)) + chunk(b'IEND', b'')


def image(rgb, w=256, h=256):
    return {'type': 'image_url', 'image_url': {'url': 'data:image/png;base64,' + base64.b64encode(png(rgb, w, h)).decode()}}


def text(value):
    return {'type': 'text', 'text': value}


def body(content, stream=False, tokens=32):
    return {'model': 'qwen3.8-flash-next', 'messages': [{'role': 'user', 'content': content}], 'temperature': 0,
            'max_tokens': tokens, 'stream': stream, 'chat_template_kwargs': {'enable_thinking': False}}


def corpus():
    a, b, d = image((255, 0, 0), 224, 224), image((0, 0, 255), 448, 112), image((0, 255, 0), 112, 448)
    cases = [('text-only', body('Describe nothing.')), ('image-only', body([a])),
             ('leading-image', body([a, text('Describe.')])), ('trailing-image', body([text('Describe this:'), a])),
             ('adjacent', body([a, b, d])), ('duplicates', body([a, a, a])),
             ('interleaved', body([text('one\n'), a, text('\ntwo '), b, text('three'), a, text(' end')])),
             ('odd-aspect', body([image((255, 255, 0), 333, 197), d])),
             ('large-resize', body([image((0, 255, 255), 2048, 1024)])),
             ('minimum-resize', body([image((255, 0, 0), 32, 32)])),
             ('literals', body([text('<|image_pad|> <|vision_start|> <think> <tool_call>'), a,
                                text('literal <__media__> <|im_end|>'), b, text('<|vision_end|>')])),
             ('utf8-boundaries', body([text('é🙂漢字\n'), a, text('\n\n العربية é'), b, text('!?')]))]
    history = body([text('Compare.'), b])
    history['messages'] = [{'role': 'user', 'content': [text('First:'), a]},
                           {'role': 'assistant', 'content': '<think>Consider carefully.</think>Earlier view.'}, history['messages'][0]]
    cases.append(('assistant-history', history))
    tools = body([text('Inspect.'), a])
    tools['tools'] = [{'type': 'function', 'function': {'name': 'inspect', 'description': 'literal <|image_pad|>',
                                                     'parameters': {'type': 'object', 'properties': {}}}}]
    cases.append(('tools-definition', tools))
    tool_history = json.loads(json.dumps(tools))
    tool_history['messages'] += [{'role': 'assistant', 'content': '', 'tool_calls': [{'id': 'call_1', 'type': 'function',
                                 'function': {'name': 'inspect', 'arguments': '{}'}}]},
                                {'role': 'tool', 'tool_call_id': 'call_1', 'content': [text('Tool image.'), b]},
                                {'role': 'user', 'content': 'Describe both. <|im_start|> is literal.'}]
    cases += [('tool-image-history', tool_history), ('long-composition', body([text('x' * 8192), a, text('y' * 8192)]))]
    for name, rgb in [('cache-a', (255, 0, 0)), ('cache-a-hit', (255, 0, 0)), ('cache-b', (0, 0, 255)),
                      ('cache-c', (0, 255, 0)), ('cache-a-evicted', (255, 0, 0)), ('cache-a-restart', (255, 0, 0))]:
        cases.append((name, body([text('Describe.'), image(rgb)])))
    return cases
